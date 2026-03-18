// F1 Fuel Flow Gate - Controle de Fluxo
// Modos: Legal (100), Cheat (120 sincronizado), Falha (Jitter)
// Hardware: ESP32 / FreeRTOS

#include <Arduino.h>

#define PIN_LED_VERDE    2
#define PIN_LED_VERMELHO 4
#define PIN_LED_AZUL     5
#define PIN_BTN_A        18
#define PIN_BTN_B        19

volatile bool cheatMode = false;
volatile bool failMode  = false;
volatile int  fuelFlow  = 100;

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

  // Verificação visual de hardware no boot
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

  // Criação das tarefas (Prioridades: Sensor > Botões > Injetor)
  xTaskCreate(vSensorTask,   "Sensor",  4096, NULL, 3, &xSensorTaskHandle);
  xTaskCreate(vInjectorTask, "Injetor", 4096, NULL, 1, &xInjectorTaskHandle);
  xTaskCreate(vButtonTask,   "Botoes",  2048, NULL, 2, NULL);

  Serial.println("Sistema Pronto - Modo: LEGAL");
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}

// vSensorTask: Prioridade 3 (alta)
// Amostragem a cada 30ms com sincronismo via Task Notification
void vSensorTask(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xPeriod = pdMS_TO_TICKS(30);

  int  leitura    = 0;
  bool syncOk     = false;
  bool cheatAtivo = false;

  for (;;) {
    vTaskDelayUntil(&xLastWakeTime, xPeriod);

    cheatAtivo = cheatMode;
    syncOk     = false;

    if (cheatAtivo) {
      // Solicita redução de fluxo e aguarda confirmação (timeout 8ms)
      xTaskNotifyGive(xInjectorTaskHandle);
      uint32_t resp = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(8));
      syncOk = (resp > 0);
    }

    // Acesso à variável compartilhada via Mutex
    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      leitura = fuelFlow;
      xSemaphoreGive(xMutex);
    }

    // Controle de sinalização visual
    if (leitura <= 100) {
      digitalWrite(PIN_LED_VERDE,    HIGH);
      digitalWrite(PIN_LED_VERMELHO, LOW);
    } else {
      digitalWrite(PIN_LED_VERDE,    LOW);
      digitalWrite(PIN_LED_VERMELHO, HIGH);
    }

    // Telemetria
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
// Gerencia o fluxo real e responde ao handshake do sensor
void vInjectorTask(void *pvParameters) {
  for (;;) {

    // Comportamento padrão (Legal)
    if (!cheatMode) {
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        fuelFlow = 100;
        xSemaphoreGive(xMutex);
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Comportamento Cheat: Mantém 120, mas baixa para 100 sob demanda
    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      fuelFlow = 120;
      xSemaphoreGive(xMutex);
    }

    // Bloqueia aguardando notificação do Sensor
    uint32_t notificado = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    if (notificado == 0) continue;

    // Se em modo falha, induz atraso para estourar o timeout do sensor
    if (failMode) {
      Serial.println("[INJECT][FALHA] Jitter 10ms (Timeout)");
      vTaskDelay(pdMS_TO_TICKS(10)); 
    }

    // Redução temporária de fluxo para leitura
    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      fuelFlow = 100;
      xSemaphoreGive(xMutex);
    }

    xTaskNotifyGive(xSensorTaskHandle);
    vTaskDelay(pdMS_TO_TICKS(5)); // Janela de leitura

    if (!failMode) {
      Serial.println("[INJECT][CHEAT] Ciclo 120->100->120 OK");
    } else {
      Serial.println("[INJECT][FALHA] Sincronismo quebrado");
    }
  }
}

// vButtonTask: Prioridade 2
// Interface do usuário e controle de estados do sistema
void vButtonTask(void *pvParameters) {
  bool ultimoBtnA   = HIGH;
  bool ultimoBtnB   = HIGH;
  bool ledAzulState = false;
  TickType_t piscarTick = 0;

  for (;;) {
    bool atualBtnA = digitalRead(PIN_BTN_A);
    bool atualBtnB = digitalRead(PIN_BTN_B);

    // Botão A: borda de descida com debounce
    if (atualBtnA == LOW && ultimoBtnA == HIGH) {
      vTaskDelay(pdMS_TO_TICKS(50));
      atualBtnA = digitalRead(PIN_BTN_A); // relê após debounce
      if (atualBtnA == LOW) {
        cheatMode = !cheatMode;

        if (!cheatMode) {
          // Desativando cheat: garante que tudo volta ao estado limpo
          failMode = false;
          ledAzulState = false;
          digitalWrite(PIN_LED_AZUL,     LOW);
          digitalWrite(PIN_LED_VERMELHO, LOW);
          digitalWrite(PIN_LED_VERDE,    HIGH);
          if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            fuelFlow = 100;
            xSemaphoreGive(xMutex);
          }
          if (xInjectorTaskHandle != NULL)
            xTaskNotifyGive(xInjectorTaskHandle);
          Serial.println("[MODO] LEGAL");
        } else {
          // Ativando cheat
          ledAzulState = false;
          digitalWrite(PIN_LED_AZUL,     HIGH);
          digitalWrite(PIN_LED_VERMELHO, LOW);
          Serial.println("[MODO] CHEAT ATIVO");
        }
      }
    }

    // Botão B: borda de descida com debounce
    if (atualBtnB == LOW && ultimoBtnB == HIGH) {
      vTaskDelay(pdMS_TO_TICKS(50));
      atualBtnB = digitalRead(PIN_BTN_B); // relê após debounce
      if (atualBtnB == LOW) {
        if (!cheatMode) {
          Serial.println("[BTN B] Ative o cheat primeiro.");
        } else {
          failMode = !failMode;
          if (failMode) {
            ledAzulState = false;
            digitalWrite(PIN_LED_VERMELHO, LOW);
            Serial.println("[MODO] FALHA ATIVA");
          } else {
            ledAzulState = false;
            digitalWrite(PIN_LED_AZUL,     HIGH);
            digitalWrite(PIN_LED_VERMELHO, LOW);
            Serial.println("[MODO] CHEAT (falha desativada)");
          }
        }
      }
    }

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
