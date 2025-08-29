#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include <inttypes.h> 

/* CONFIGURAÇÕES */
#define SENSOR_TASK_PRIO    6
#define COMM_TASK_PRIO      4
#define LOG_TASK_PRIO       2
#define MONITOR_TASK_PRIO   5

#define SENSOR_STACK_WORDS  4096
#define COMM_STACK_WORDS    4096
#define LOG_STACK_WORDS     3072
#define MONITOR_STACK_WORDS 4096

#define QUEUE_LEN           10
#define QUEUE_ITEM_SIZE     sizeof(message_t)

/* TIMEOUTS */
#define QUEUE_RX_TMO_MS     1000
#define MONITOR_PERIOD_MS   2000
#define MAX_TIMEOUTS_BEFORE_RECOVERY 5

typedef struct {
    uint32_t seq;
    int value;
} message_t;

/* Handles globais */
static QueueHandle_t g_queue = NULL;
static TaskHandle_t g_task_sensor = NULL;
static TaskHandle_t g_task_comm   = NULL;
static TaskHandle_t g_task_log    = NULL;

/* Heartbeats para monitoramento */
static volatile uint32_t sensor_heartbeat = 0;
static volatile uint32_t comm_heartbeat   = 0;

/* -------- TAREFAS -------- */

/* Tarefa de sensor */
void task_sensor(void *pv) {
    esp_task_wdt_add(NULL);

    uint32_t seq = 0;
    for (;;) {
        message_t msg = { .seq = seq++, .value = rand() % 1000 };

        if (xQueueSend(g_queue, &msg, 0) != pdTRUE) {
            printf("[SENSOR] Fila cheia, mensagem perdida (seq=%" PRIu32 ")\n", msg.seq);
        } else {
            sensor_heartbeat = xTaskGetTickCount();
            printf("[SENSOR] Enviada mensagem seq=%" PRIu32 " valor=%d\n", msg.seq, msg.value);
        }

        UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
        if (watermark < 100) {
            printf("[SENSOR] Atenção: pouca memória de pilha restante (%u words)\n", watermark);
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* Tarefa de comunicação */
void task_comm(void *pv) {
    esp_task_wdt_add(NULL);

    size_t buf_size = 256;
    void *dma_buf = malloc(buf_size);
    if (!dma_buf) {
        printf("[COMM] Crítico: não foi possível alocar buffer de %d bytes. Finalizando tarefa.\n", (int)buf_size);
        vTaskDelete(NULL);
        return;
    }
    memset(dma_buf, 0, buf_size);

    int timeouts = 0;
    for (;;) {
        message_t msg;
        TickType_t tmo = pdMS_TO_TICKS(QUEUE_RX_TMO_MS);
        if (tmo == 0) tmo = 1;

        if (xQueueReceive(g_queue, &msg, tmo) == pdTRUE) {
            timeouts = 0;
            comm_heartbeat = xTaskGetTickCount();
            snprintf((char*)dma_buf, buf_size, "SEQ:%" PRIu32 " VAL:%d", msg.seq, msg.value);
            printf("[COMM] Transmitindo: %s\n", (char*)dma_buf);
        } else {
            timeouts++;
            printf("[COMM] Timeout na fila (%d)\n", timeouts);

            if (timeouts == 2) {
                printf("[COMM] Tentativa de recuperação leve (limpar caches locais)\n");
            } else if (timeouts == 4) {
                printf("[COMM] Recuperação moderada: resetando fila\n");
                xQueueReset(g_queue);
            } else if (timeouts >= MAX_TIMEOUTS_BEFORE_RECOVERY) {
                printf("[COMM] Falha persistente, solicitando recriação da tarefa pelo monitor\n");
                break;
            }
        }

        size_t free_heap = xPortGetFreeHeapSize();
        size_t min_heap  = xPortGetMinimumEverFreeHeapSize();
        if (free_heap < 20 * 1024) {
            printf("[COMM] Atenção: pouca memória livre (%u bytes, mínimo histórico %u)\n", (unsigned int)free_heap, (unsigned int)min_heap);
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (dma_buf) {
        free(dma_buf);
        dma_buf = NULL;
    }
    printf("[COMM] Tarefa finalizada para permitir que o monitor a recrie.\n");
    vTaskDelete(NULL);
}

/* Tarefa de logging */
void task_log(void *pv) {
    static char logbuf[512];
    for (;;) {
        snprintf(logbuf, sizeof(logbuf), "LOG - Sensor=%u Comm=%u",
                 (unsigned int)sensor_heartbeat, (unsigned int)comm_heartbeat);
        printf("[LOG] %s\n", logbuf);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Tarefa monitor */
void task_monitor(void *pv) {
    int comm_restart_count = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));
        TickType_t now = xTaskGetTickCount();

        if (now - sensor_heartbeat > pdMS_TO_TICKS(2 * MONITOR_PERIOD_MS)) {
            printf("[MONITOR] Sensor inativo. Reiniciando tarefa...\n");
            if (g_task_sensor) {
                vTaskDelete(g_task_sensor);
                g_task_sensor = NULL;
            }
            xTaskCreatePinnedToCore(task_sensor, "task_sensor", SENSOR_STACK_WORDS, NULL, SENSOR_TASK_PRIO, &g_task_sensor, 1);
            sensor_heartbeat = xTaskGetTickCount();
        }

        if (g_task_comm == NULL || (now - comm_heartbeat > pdMS_TO_TICKS(5 * MONITOR_PERIOD_MS))) {
            printf("[MONITOR] Comunicação inativa. Recriando tarefa...\n");
            if (g_task_comm) {
                vTaskDelete(g_task_comm);
                g_task_comm = NULL;
            }
            xTaskCreatePinnedToCore(task_comm, "task_comm", COMM_STACK_WORDS, NULL, COMM_TASK_PRIO, &g_task_comm, 1);

            comm_restart_count++;
            if (comm_restart_count >= 3) {
                size_t free_heap = xPortGetFreeHeapSize();
                if (free_heap < 16 * 1024) {
                    printf("[MONITOR] Memória crítica detectada (%u bytes). Reiniciando sistema.\n", (unsigned int)free_heap);
                    esp_restart();
                }
            }
            comm_heartbeat = xTaskGetTickCount();
        }

        size_t free = xPortGetFreeHeapSize();
        size_t min  = xPortGetMinimumEverFreeHeapSize();
        printf("[MONITOR] Memória livre=%u bytes (mínimo histórico %u)\n", (unsigned int)free, (unsigned int)min);

        if (min < 8 * 1024) {
            printf("[MONITOR] Memória mínima histórica muito baixa. Reiniciando sistema...\n");
            esp_restart();
        }
    }
}

/* -------- app_main: cria fila e tarefas -------- */
void app_main(void) {
    printf("Iniciando exemplo robusto com FreeRTOS no ESP32\n");

    g_queue = xQueueCreate(QUEUE_LEN, QUEUE_ITEM_SIZE);
    if (!g_queue) {
        printf("Falha ao criar fila. Reiniciando sistema...\n");
        esp_restart();
    }

    xTaskCreatePinnedToCore(task_sensor, "task_sensor", SENSOR_STACK_WORDS, NULL, SENSOR_TASK_PRIO, &g_task_sensor, 1);
    xTaskCreatePinnedToCore(task_comm,   "task_comm",   COMM_STACK_WORDS,   NULL, COMM_TASK_PRIO,   &g_task_comm,   1);
    xTaskCreatePinnedToCore(task_log,    "task_log",    LOG_STACK_WORDS,    NULL, LOG_TASK_PRIO,    &g_task_log,    1);
    xTaskCreatePinnedToCore(task_monitor,"task_monitor",MONITOR_STACK_WORDS,NULL, MONITOR_TASK_PRIO, NULL,           1);

    printf("Todas as tarefas foram criadas com sucesso\n");
}
