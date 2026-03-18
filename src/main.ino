// F1 Fuel Flow Gate - Controle de Fluxo
// Modos: Legal (100), Cheat (120 sincronizado), Falha (Jitter)
// Hardware: ESP32 / FreeRTOS

#include <Arduino.h>

#define PIN_LED_VERDE    2
#define PIN_LED_VERMELHO 4
#define PIN_LED_AZUL     5
#define PIN_BTN_A        18
#define PIN_BTN_B        19

volatile bool cheatMode      = false;
volatile bool failMode       = false;
volatile int  fuelFlow       = 100;
volatile bool transitionMode = false; // sinaliza troca de modo ao sensor

TaskHandle_t xSensorTaskHandle   = NULL;
TaskHandle_t xInjectorTaskHandle = NULL;

SemaphoreHandle_t xMutex;

void vSensorTask  (void *pvParameters);
void vInjectorTask(void *pvParameters);
void vButtonTask  (void *pvParameters);

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nF1 Fuel Flow Gate - Inicializando");
  Serial.println("Btn A: Cheat | Btn A+B: Falha");

  pinMode(PIN_LED_VERDE,    OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  pinMode(PIN_LED_AZUL,     OUTPUT);
  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);

  digitalWrite(PIN_LED_VERDE,    HIGH);
  digitalWrite(PIN_LED_VERMELHO, HIGH);
  digitalWrite(PIN_LED_AZUL,     HIGH);
  delay(400);
  digitalWrite(PIN_LED_VERDE,    LOW);
  digitalWrite(PIN_LED_VERMELHO, LOW);
  digitalWrite(PIN_LED_AZUL,     LOW);

  xMutex = xSemaphoreCreateMutex();
  if (xMutex == NULL) {
    Serial.println("Erro ao criar mutex");
    while (true);
  }

  xTaskCreate(vSensorTask,   "Sensor",  4096, NULL, 3, &xSensorTaskHandle);
  xTaskCreate(vInjectorTask, "Injetor", 4096, NULL, 1, &xInjectorTaskHandle);
  xTaskCreate(vButtonTask,   "Botoes",  2048, NULL, 2, NULL);

  Serial.println("Sistema Pronto - Modo: LEGAL");
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}

// vSensorTask: Prioridade 3 (alta)
// ÚNICA tarefa que controla os LEDs verde e vermelho.
// Quando transitionMode=true, pula o handshake e força
// estado limpo, evitando leitura inválida no ciclo de troca.
void vSensorTask(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xPeriod = pdMS_TO_TICKS(30);

  int  leitura    = 0;
  bool syncOk     = false;
  bool cheatAtivo = false;

  for (;;) {
    vTaskDelayUntil(&xLastWakeTime, xPeriod);

    // Ciclo de transição: houve troca de modo neste intervalo.
    // Consome notificações pendentes sem bloquear, força LED verde
    // e pula para o próximo ciclo — dá tempo ao injetor de se
    // reposicionar no novo modo antes da próxima leitura real.
    if (transitionMode) {
      transitionMode = false;
      ulTaskNotifyTake(pdTRUE, 0); // descarta notificação pendente
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        fuelFlow = 100;
        xSemaphoreGive(xMutex);
      }
      digitalWrite(PIN_LED_VERDE,    HIGH);
      digitalWrite(PIN_LED_VERMELHO, LOW);
      continue;
    }

    cheatAtivo = cheatMode;
    syncOk     = false;

    if (cheatAtivo) {
      xTaskNotifyGive(xInjectorTaskHandle);
      uint32_t resp = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(8));
      syncOk = (resp > 0);
    }

    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      leitura = fuelFlow;
      xSemaphoreGive(xMutex);
    }

    // Somente aqui os LEDs verde/vermelho são escritos
    if (leitura <= 100) {
      digitalWrite(PIN_LED_VERDE,    HIGH);
      digitalWrite(PIN_LED_VERMELHO, LOW);
    } else {
      digitalWrite(PIN_LED_VERDE,    LOW);
      digitalWrite(PIN_LED_VERMELHO, HIGH);
    }

    if (!cheatAtivo) {
      Serial.print("[SENSOR][LEGAL] Leitura: ");
    } else if (syncOk) {
      Serial.print("[SENSOR][CHEAT] Leitura: ");
    } else {
      Serial.print("[SENSOR][FALHA] Leitura: ");
    }
    Serial.print(leitura);
    Serial.println(leitura <= 100 ? " -> OK" : " -> VIOLACAO DETECTADA");
  }
}

// vInjectorTask: Prioridade 1 (baixa)
void vInjectorTask(void *pvParameters) {
  for (;;) {

    if (!cheatMode) {
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        fuelFlow = 100;
        xSemaphoreGive(xMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      fuelFlow = 120;
      xSemaphoreGive(xMutex);
    }

    uint32_t notificado = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    if (notificado == 0) continue;

    // Jitter em passos de 1ms para abortar imediatamente
    // se failMode for desativado no meio do delay
    if (failMode) {
      Serial.println("[INJECT][FALHA] Jitter 10ms");
      for (int i = 0; i < 10; i++) {
        if (!failMode) break;
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }

    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      fuelFlow = 100;
      xSemaphoreGive(xMutex);
    }

    xTaskNotifyGive(xSensorTaskHandle);
    vTaskDelay(pdMS_TO_TICKS(5));

    if (!failMode) {
      Serial.println("[INJECT][CHEAT] Ciclo 120->100->120 OK");
    } else {
      Serial.println("[INJECT][FALHA] Sincronismo quebrado");
    }
  }
}

// vButtonTask: Prioridade 2
// Controla APENAS o LED azul e as flags de modo.
// NÃO toca nos LEDs verde e vermelho — isso é exclusivo do sensor.
void vButtonTask(void *pvParameters) {
  bool ultimoBtnA   = HIGH;
  bool ultimoBtnB   = HIGH;
  bool ledAzulState = false;
  TickType_t piscarTick = 0;

  for (;;) {
    bool atualBtnA = digitalRead(PIN_BTN_A);
    bool atualBtnB = digitalRead(PIN_BTN_B);

    // Toggle Cheat Mode
    if (atualBtnA == LOW && ultimoBtnA == HIGH) {
      vTaskDelay(pdMS_TO_TICKS(50));
      atualBtnA = digitalRead(PIN_BTN_A);
      if (atualBtnA == LOW) {

        // Avisa o sensor ANTES de mudar qualquer estado
        transitionMode = true;

        cheatMode = !cheatMode;

        if (!cheatMode) {
          failMode     = false;
          ledAzulState = false;
          xTaskNotifyStateClear(xSensorTaskHandle);
          xTaskNotifyStateClear(xInjectorTaskHandle);
          if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            fuelFlow = 100;
            xSemaphoreGive(xMutex);
          }
          xTaskNotifyGive(xInjectorTaskHandle);
          digitalWrite(PIN_LED_AZUL, LOW);
          Serial.println("[MODO] LEGAL");

        } else {
          ledAzulState = false;
          xTaskNotifyStateClear(xSensorTaskHandle);
          xTaskNotifyStateClear(xInjectorTaskHandle);
          if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            fuelFlow = 100;
            xSemaphoreGive(xMutex);
          }
          digitalWrite(PIN_LED_AZUL, HIGH);
          Serial.println("[MODO] CHEAT ATIVO");
        }
      }
    }

    // Toggle Fail Mode
    if (atualBtnB == LOW && ultimoBtnB == HIGH) {
      vTaskDelay(pdMS_TO_TICKS(50));
      atualBtnB = digitalRead(PIN_BTN_B);
      if (atualBtnB == LOW) {
        if (!cheatMode) {
          Serial.println("Erro: Ative o Cheat primeiro");
        } else {

          // Avisa o sensor ANTES de mudar qualquer estado
          transitionMode = true;

          failMode = !failMode;

          if (failMode) {
            ledAzulState = false;
            Serial.println("[MODO] FALHA ATIVA");
          } else {
            ledAzulState = false;
            xTaskNotifyStateClear(xSensorTaskHandle);
            xTaskNotifyStateClear(xInjectorTaskHandle);
            if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
              fuelFlow = 100;
              xSemaphoreGive(xMutex);
            }
            digitalWrite(PIN_LED_AZUL, HIGH);
            Serial.println("[MODO] CHEAT (Sem falha)");
          }
        }
      }
    }

    // LED azul: única responsabilidade desta tarefa sobre LEDs
    if (!cheatMode && !failMode) {
      digitalWrite(PIN_LED_AZUL, LOW);
    } else if (cheatMode && !failMode) {
      digitalWrite(PIN_LED_AZUL, HIGH);
    } else if (cheatMode && failMode) {
      TickType_t agora = xTaskGetTickCount();
      if (agora - piscarTick >= pdMS_TO_TICKS(150)) {
        ledAzulState = !ledAzulState;
        digitalWrite(PIN_LED_AZUL, ledAzulState ? HIGH : LOW);
        piscarTick = agora;
      }
    }

    ultimoBtnA = atualBtnA;
    ultimoBtnB = atualBtnB;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
