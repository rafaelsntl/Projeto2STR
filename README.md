# F1 Fuel Flow Gate

> Simulação embarcada de fiscalização e burla do limite de consumo de combustível da Fórmula 1, implementada com FreeRTOS no ESP32 (Wokwi).

---

## Sumário

- [Contexto e Motivação](#contexto-e-motivação)
- [Objetivo do Projeto](#objetivo-do-projeto)
- [Arquitetura do Sistema](#arquitetura-do-sistema)
- [Tarefas FreeRTOS](#tarefas-freertos)
- [Mecanismo de Sincronização](#mecanismo-de-sincronização)
- [Modos de Operação](#modos-de-operação)
- [Hardware Virtual (Wokwi)](#hardware-virtual-wokwi)
- [Como Executar](#como-executar)
- [Como Testar — Sequência Completa](#como-testar--sequência-completa)
- [Estrutura de Arquivos](#estrutura-de-arquivos)
- [Parâmetros Configuráveis](#parâmetros-configuráveis)
- [Decisões de Projeto](#decisões-de-projeto)

---

## Contexto e Motivação

No regulamento técnico da Fórmula 1, a FIA impõe um limite rigoroso de **100 kg/h** de consumo de combustível durante as corridas. O monitoramento é feito por um **sensor ultrassônico** que amostra o fluxo de combustível em alta frequência — **2000 vezes por minuto** (uma leitura a cada 30 ms).

Em 2019, surgiu uma polêmica técnica envolvendo uma equipe que teria descoberto a **frequência de amostragem exata** do sensor oficial. A suspeita era de que o sistema de injeção operava com fluxo superior a 100 kg/h na maior parte do tempo, mas reduzia esse valor **precisamente nos milissegundos em que o sensor realizava cada leitura**. Dessa forma, todas as amostras registravam um valor dentro do limite legal — mas o consumo médio real era ilegal.

Este projeto reproduz esse cenário em software embarcado, demonstrando na prática os conceitos de sincronização, priorização de tarefas e temporização precisa com FreeRTOS.

---

## Objetivo do Projeto

Implementar um sistema embarcado com FreeRTOS que simule três componentes interagindo em tempo real:

| Componente | Tarefa | Papel |
|---|---|---|
| Sensor da FIA | `vSensorTask` | Fiscaliza o fluxo a cada 30 ms |
| Sistema de injeção | `vInjectorTask` | Controla o fluxo de combustível |
| Interface de controle | `vButtonTask` | Lê botões e alterna modos |

O sistema demonstra três modos de operação: **Legal**, **Cheat** (fraude bem-sucedida) e **Falha Proposital** (fraude detectada), controlados por dois botões físicos.

---

## Arquitetura do Sistema

```
┌─────────────────────────────────────────────────────────┐
│                    FreeRTOS — ESP32                      │
│                                                          │
│   ┌─────────────────┐      ┌──────────────────────┐     │
│   │  vSensorTask    │      │   vInjectorTask       │     │
│   │  Prioridade: 3  │◄────►│   Prioridade: 1       │     │
│   │  Período: 30ms  │      │   (bloqueante)        │     │
│   └────────┬────────┘      └──────────┬────────────┘     │
│            │  Task Notifications      │                  │
│            └──────────────────────────┘                  │
│                         │                                │
│              ┌──────────▼──────────┐                     │
│              │   fuelFlow (int)    │                     │
│              │   Mutex protegido   │                     │
│              └─────────────────────┘                     │
│                                                          │
│   ┌─────────────────┐                                    │
│   │  vButtonTask    │  → cheatMode, failMode (volatile)  │
│   │  Prioridade: 2  │                                    │
│   └─────────────────┘                                    │
└─────────────────────────────────────────────────────────┘
         │               │              │
     LED Verde       LED Vermelho    LED Azul
     (fluxo OK)      (violação)     (cheat/falha)
```

---

## Tarefas FreeRTOS

### `vSensorTask` — Prioridade 3 (alta)

Simula o sensor ultrassônico da FIA.

- Usa `vTaskDelayUntil` para garantir período **exato e não-acumulativo** de 30 ms.
- No **Modo Cheat/Falha**: executa o handshake de dois passos antes de ler.
  - Envia notificação ao injetor: *"vou ler em breve"*.
  - Aguarda confirmação do injetor com **timeout de 8 ms**.
  - Se confirmado a tempo → lê 100 → LED Verde.
  - Se timeout expirou → lê 120 → LED Vermelho (VIOLAÇÃO).

### `vInjectorTask` — Prioridade 1 (baixa)

Simula o sistema de injeção de combustível.

- **Modo Legal**: escreve `fuelFlow = 100` e dorme 10 ms.
- **Modo Cheat**: escreve `fuelFlow = 120`, bloqueia aguardando notificação do sensor. Ao ser notificado, baixa para 100, confirma ao sensor, aguarda 5 ms e volta a 120.
- **Modo Falha**: igual ao Cheat, mas insere `vTaskDelay(10 ms)` antes de baixar o fluxo. Como 10 ms > timeout de 8 ms do sensor, o sensor lê 120.

### `vButtonTask` — Prioridade 2

Gerencia os botões com debounce por software (50 ms).

- **Botão A**: toggle de `cheatMode`. Desligar o cheat também desliga o failMode e garante retorno seguro do injetor ao Modo Legal.
- **Botão B**: toggle de `failMode` (só funciona com `cheatMode` ativo).
- Controla o LED Azul: apagado = Legal, fixo = Cheat, piscando (150 ms) = Falha.

---

## Mecanismo de Sincronização

O handshake usa **Task Notifications** — o mecanismo de sinalização mais leve do FreeRTOS, sem alocação de heap e mais rápido que semáforos.

### Modo Cheat (fraude bem-sucedida):

```
t = 30ms  ← sensor acorda
  Sensor:   xTaskNotifyGive(injetor)     ← "vou ler"
  Sensor:   ulTaskNotifyTake(timeout=8ms) ← bloqueia

  Injetor:  acorda, fuelFlow = 100
  Injetor:  xTaskNotifyGive(sensor)      ← "pode ler"

t ≈ 31ms
  Sensor:   lê fuelFlow = 100 ✓ → LED Verde

t ≈ 36ms
  Injetor:  fuelFlow = 120 (volta ao valor real)
```

### Modo Falha (fraude detectada):

```
t = 30ms  ← sensor acorda
  Sensor:   xTaskNotifyGive(injetor)
  Sensor:   ulTaskNotifyTake(timeout=8ms) ← bloqueia

  Injetor:  acorda
  Injetor:  vTaskDelay(10ms) ← JITTER INTENCIONAL

t ≈ 38ms  ← timeout de 8ms expirou
  Sensor:   avança sem confirmação
  Sensor:   lê fuelFlow = 120 ✗ → LED Vermelho (VIOLAÇÃO)
```

---

## Modos de Operação

| Modo | Btn A | Btn B | `fuelFlow` real | Sensor lê | LED Verde | LED Vermelho | LED Azul |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **Legal** | OFF | — | 100 | 100 | ✅ | — | — |
| **Cheat** | ON | OFF | 120 | 100 | ✅ | — | Fixo |
| **Falha** | ON | ON | 120 | 120 | — | ✅ | Piscando |

---

## Hardware Virtual (Wokwi)

O projeto roda no [Wokwi](https://wokwi.com) — simulador online de ESP32 com suporte a FreeRTOS nativo.

### Componentes e conexões

| Componente | GPIO | Função |
|---|:---:|---|
| LED Verde | 2 | Status OK (fluxo ≤ 100) |
| LED Vermelho | 4 | Violação detectada (fluxo > 100) |
| LED Azul | 5 | Indicador de modo cheat/falha |
| Botão A | 18 | Ativa/desativa cheat mode |
| Botão B | 19 | Ativa/desativa fail mode |

Todos os LEDs utilizam resistor de **220 Ω** em série. Os botões utilizam **pull-up interno** (`INPUT_PULLUP`).

### `diagram.json`

```json
{
  "version": 1,
  "author": "F1 Fuel Flow Gate",
  "editor": "wokwi",
  "parts": [
    { "type": "wokwi-esp32-devkit-v1", "id": "esp", "top": 150, "left": 0, "attrs": {} },
    { "type": "wokwi-led", "id": "ledVerde",    "top": 0,   "left": 50,  "attrs": { "color": "green" } },
    { "type": "wokwi-led", "id": "ledVermelho", "top": 0,   "left": 120, "attrs": { "color": "red"   } },
    { "type": "wokwi-led", "id": "ledAzul",     "top": 0,   "left": 190, "attrs": { "color": "blue"  } },
    { "type": "wokwi-resistor", "id": "r1", "top": 70, "left": 50,  "attrs": { "value": "220" } },
    { "type": "wokwi-resistor", "id": "r2", "top": 70, "left": 120, "attrs": { "value": "220" } },
    { "type": "wokwi-resistor", "id": "r3", "top": 70, "left": 190, "attrs": { "value": "220" } },
    { "type": "wokwi-pushbutton", "id": "btnA", "top": 150, "left": 320, "attrs": { "color": "green" } },
    { "type": "wokwi-pushbutton", "id": "btnB", "top": 230, "left": 320, "attrs": { "color": "red"   } }
  ],
  "connections": [
    [ "esp:D2", "r1:1", "green", [] ],
    [ "r1:2",   "ledVerde:A",    "green", [] ],
    [ "ledVerde:C",    "esp:GND.1", "black", [] ],

    [ "esp:D4", "r2:1", "red", [] ],
    [ "r2:2",   "ledVermelho:A", "red", [] ],
    [ "ledVermelho:C", "esp:GND.1", "black", [] ],

    [ "esp:D5", "r3:1", "blue", [] ],
    [ "r3:2",   "ledAzul:A",    "blue", [] ],
    [ "ledAzul:C",     "esp:GND.1", "black", [] ],

    [ "esp:D18", "btnA:1.l", "green", [] ],
    [ "btnA:2.l", "esp:GND.1", "black", [] ],

    [ "esp:D19", "btnB:1.l", "red", [] ],
    [ "btnB:2.l", "esp:GND.1", "black", [] ]
  ],
  "dependencies": {}
}
```

---

## Como Executar

1. Acesse [wokwi.com](https://wokwi.com) e crie um novo projeto **ESP32**.
2. Substitua o conteúdo de `diagram.json` pelo JSON acima.
3. Substitua o conteúdo de `sketch.ino` pelo código em `src/main.ino`.
4. Clique em **"Start Simulation"**.
5. Ao iniciar, os três LEDs piscam uma vez juntos — confirmação de que o sistema está rodando.
6. O LED verde acende e permanece fixo — sistema no Modo Legal.

---

## Como Testar — Sequência Completa

### Teste 1 — Modo Legal

**Ação:** Nenhuma. Sistema recém-iniciado.

**Esperado:** LED Verde aceso, LED Vermelho apagado, LED Azul apagado.

---

### Teste 2 — Ativar Modo Cheat

**Ação:** Pressione o **Botão A** (verde).

**Esperado:** LED Verde continua aceso, LED Azul acende fixo, LED Vermelho permanece apagado.

> A fraude está ativa mas invisível ao sensor — o injetor sincroniza o fluxo exatamente na janela de leitura.

---

### Teste 3 — Ativar Modo Falha

**Ação:** Com Botão A ativo, pressione o **Botão B** (vermelho).

**Esperado:** LED Vermelho acende, LED Verde apaga, LED Azul pisca.

> O jitter de 10ms ultrapassa o timeout de 8ms do sensor — a fraude é detectada.

---

### Teste 4 — Desativar Falha

**Ação:** Pressione o **Botão B** novamente.

**Esperado:** LED Vermelho apaga, LED Verde acende, LED Azul volta a fixo.

---

### Teste 5 — Desativar Cheat

**Ação:** Pressione o **Botão A**.

**Esperado:** LED Azul apaga, LED Verde aceso, sistema no Modo Legal.

---

## Estrutura de Arquivos

```
f1-fuel-flow-gate/
├── README.md
├── diagram.json
└── src/
    └── main.ino
```

---

## Parâmetros Configuráveis

| Parâmetro | Valor padrão | Descrição |
|---|:---:|---|
| Período do sensor | `30 ms` | Frequência de amostragem (2000x/min) |
| Timeout do sensor | `8 ms` | Espera máxima pela confirmação do injetor |
| Delay pós-confirmação | `5 ms` | Margem para leitura concluir antes de subir para 120 |
| Jitter no modo falha | `10 ms` | Atraso intencional — deve ser maior que o timeout |
| Debounce dos botões | `50 ms` | Estabilização do sinal do botão |
| Piscar LED Azul | `150 ms` | Half-period do LED no Modo Falha |

> **Experimento:** alterar o jitter para 6 ms (menor que o timeout de 8 ms) faz o cheat voltar a funcionar mesmo com o Botão B ativo. Isso demonstra que o threshold de detecção é a diferença entre os dois valores.

---

## Decisões de Projeto

### Por que Task Notifications em vez de semáforos?

Task Notifications operam diretamente no TCB (Task Control Block) da tarefa destino, sem alocar nenhum objeto adicional no heap. São mais rápidas que semáforos binários e consomem zero memória extra. Para sinalização ponto a ponto entre duas tarefas com handles conhecidos, são sempre a escolha ideal.

### Por que `vTaskDelayUntil` e não `vTaskDelay`?

`vTaskDelay(30)` espera 30 ms após o fim do corpo da tarefa — se o corpo levar 2 ms, o período real fica 32 ms e esse erro acumula. `vTaskDelayUntil` desconta o tempo de execução e garante período exato. Para um sensor de fiscalização onde a frequência de amostragem é o parâmetro crítico, isso é indispensável.

### Por que o handshake tem dois passos?

Um único passo não garante ordenação: o sensor poderia ler antes de a escrita do injetor ser visível. O segundo passo garante que o sensor só avança para a leitura depois que o valor 100 está definitivamente em memória.

### Por que o mutex ainda é necessário com o handshake?

O handshake garante a **ordenação temporal**. O mutex garante a **atomicidade do acesso** — que leitura e escrita não ocorram literalmente ao mesmo tempo em núcleos diferentes do ESP32 dual-core.

### Por que `vButtonTask` tem prioridade 2?

O sensor (prioridade 3) precisa preemptar qualquer coisa durante o período crítico. O injetor (prioridade 1) deve ceder CPU sempre que outros acordam. A tarefa de botões, com prioridade 2, responde ao usuário sem interferir no par sensor-injetor.

---

## Referências

- [FreeRTOS — Task Notifications](https://www.freertos.org/RTOS-task-notifications.html)
- [FreeRTOS — vTaskDelayUntil](https://www.freertos.org/vtaskdelayuntil.html)
- [Wokwi — ESP32 Simulator](https://wokwi.com)
- FIA Technical Regulations 2019 — Article 5.10 (Fuel Mass Flow)
