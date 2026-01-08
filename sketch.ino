#include <Arduino_FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <timers.h>
#include <stdarg.h>

/*
  proiect patr tema tunel

  ideea principala
  sistemul este modelat pe evenimente, ca sa putem simula senzorii fara hardware
  un task citeste comenzi din serial si le transforma in evenimente
  un task controller aplica regulile si actualizeaza starea
  un task logger afiseaza pe serial, ca sa nu se amestece mesajele
  un task emulator poate genera automat trafic (intrari si iesiri) fara senzori reali

  reguli cerinta
  intrarea se opreste daca
  - se depaseste n=5 masini in tunel
  - exista incident in tunel (gaz sau fum)
  - butonul de panica este activ
  - operatorul blocheaza intrarea

  se permite doar iesirea din tunel in caz de incident sau panica
  adica, in acele situatii, intrarea devine blocata automat, iesirea ramane permisa
  operatorul poate bloca si iesirea separat, pentru a simula interventii externe

  comenzi serial explicate
  e1  inseamna o masina intra in tunel pe banda 1
  x1  inseamna o masina iese din tunel pe banda 1
  e2  inseamna o masina intra in tunel pe banda 2
  x2  inseamna o masina iese din tunel pe banda 2

  gas on   simuleaza detectie scurgere gaze naturale
  gas off  simuleaza revenirea la normal, fara scurgere

  smoke on  simuleaza detectie fum (posibil incendiu)
  smoke off simuleaza revenirea la normal, fara fum

  panic on  simuleaza apasarea butonului de panica (alarma)
  panic off simuleaza dezactivarea butonului de panica

  bin on   operatorul blocheaza intrarea in tunel
  bin off  operatorul deblocheaza intrarea

  bout on  operatorul blocheaza iesirea din tunel
  bout off operatorul deblocheaza iesirea

  status  afiseaza starea curenta manual
  help    afiseaza lista de comenzi

  emulator trafic
  auto on   porneste emularea automata a traficului
  auto off  opreste emularea automata

  rate 800  seteaza perioada emulatorului in milisecunde
            adica la fiecare 800ms emulatorul incearca o intrare sau o iesire

  burst 3   trimite rapid 3 evenimente de intrare (ca un val de masini)
            util pentru testarea blocarii la n masini
*/

// configurare pini led
static const uint8_t PIN_GATE_IN_LED  = 7;   // led intrare permisa - led verde
static const uint8_t PIN_GATE_OUT_LED = 6;   // led iesire permisa - led albastru
static const uint8_t PIN_ALARM_LED    = 13;  // led alarma (incident sau panica) - led rosu

// capacitate fixa n
static const uint16_t N_DEFAULT = 5;

// polling serial pentru comenzi
static const TickType_t CLI_POLL_MS = 20;

// tipuri de evenimente
enum EventType : uint8_t {
  EV_ENTRY_L1,
  EV_EXIT_L1,
  EV_ENTRY_L2,
  EV_EXIT_L2,

  EV_GAS_ON,
  EV_GAS_OFF,
  EV_SMOKE_ON,
  EV_SMOKE_OFF,
  EV_PANIC_ON,
  EV_PANIC_OFF,

  EV_OP_BLOCK_IN_ON,
  EV_OP_BLOCK_IN_OFF,
  EV_OP_BLOCK_OUT_ON,
  EV_OP_BLOCK_OUT_OFF,

  EV_STATUS_REQ,
  EV_HELP
};

struct Event {
  EventType type;
  int16_t value;
  TickType_t tick;
};

struct LogMsg {
  TickType_t tick;
  char text[96];
};

// resurse free rtos
static QueueHandle_t qEvents = NULL;
static QueueHandle_t qLog    = NULL;

static SemaphoreHandle_t mtxState = NULL;      // mutex pentru starea globala
static SemaphoreHandle_t semCapacity = NULL;   // counting semaphore pentru n locuri

// starea sistemului
struct SystemState {
  uint16_t N;
  int16_t carsInside;

  bool gas;
  bool smoke;
  bool panic;

  bool opBlockIn;
  bool opBlockOut;

  bool entryAllowed;
  bool exitAllowed;

  // setari emulator
  bool emuOn;
  uint16_t emuRateMs;
  uint8_t emuLaneToggle;
};

static SystemState S;

// log in coada, pentru ca un singur task sa scrie pe serial
static void logf(const char* fmt, ...) {
  if (!qLog) return;

  LogMsg m;
  m.tick = xTaskGetTickCount();

  va_list args;
  va_start(args, fmt);
  vsnprintf(m.text, sizeof(m.text), fmt, args);
  va_end(args);

  xQueueSend(qLog, &m, 0);
}

// calculeaza regulile de acces
static void recomputeGates_locked() {
  const bool incident = (S.gas || S.smoke);
  const bool stopAccess = incident || S.panic;
  const bool full = (S.carsInside >= (int16_t)S.N);

  // intrarea se opreste daca e plin, incident, panica sau operatorul blocheaza
  S.entryAllowed = (!stopAccess) && (!S.opBlockIn) && (!full);

  // iesirea e permisa, dar operatorul o poate bloca separat
  S.exitAllowed  = (!S.opBlockOut);
}

// aplica iesiri pe pini, ca sa se vada in wokwi
static void applyOutputs_locked() {
  digitalWrite(PIN_GATE_IN_LED,  S.entryAllowed ? HIGH : LOW);
  digitalWrite(PIN_GATE_OUT_LED, S.exitAllowed  ? HIGH : LOW);

  const bool alarm = (S.gas || S.smoke || S.panic);
  digitalWrite(PIN_ALARM_LED, alarm ? HIGH : LOW);
}

// afiseaza un snapshot al starii
static void printState() {
  xSemaphoreTake(mtxState, portMAX_DELAY);
  SystemState snap = S;
  xSemaphoreGive(mtxState);

  logf("state | n=%u cars=%d | gas=%d smoke=%d panic=%d | bin=%d bout=%d | in=%d out=%d | auto=%d rate=%ums",
       snap.N, snap.carsInside,
       snap.gas, snap.smoke, snap.panic,
       snap.opBlockIn, snap.opBlockOut,
       snap.entryAllowed, snap.exitAllowed,
       snap.emuOn, snap.emuRateMs);
}

// trimite un eveniment in coada
static void enqueueEvent(EventType t) {
  if (!qEvents) return;
  Event ev;
  ev.type = t;
  ev.value = 0;
  ev.tick = xTaskGetTickCount();

  if (xQueueSend(qEvents, &ev, 0) != pdTRUE) {
    logf("warn: qevents full, drop event=%d", (int)t);
  }
}

// task logger, singurul care scrie pe serial, ca sa nu se amestece mesajele
static void TaskLogger(void*) {
  LogMsg m;
  for (;;) {
    if (xQueueReceive(qLog, &m, portMAX_DELAY) == pdTRUE) {
      Serial.print("[");
      Serial.print((unsigned long)m.tick);
      Serial.print("] ");
      Serial.println(m.text);
    }
  }
}

// task controller, aici se modifica starea si se aplica regulile
static void TaskController(void*) {
  Event ev;

  for (;;) {
    if (xQueueReceive(qEvents, &ev, portMAX_DELAY) != pdTRUE) continue;

    bool changed = false;
    bool statusRequested = false;

    xSemaphoreTake(mtxState, portMAX_DELAY);

    switch (ev.type) {
      case EV_ENTRY_L1:
      case EV_ENTRY_L2: {
          recomputeGates_locked();
          const int lane = (ev.type == EV_ENTRY_L1) ? 1 : 2;

          if (!S.entryAllowed) {
            logf("entry blocked (lane %d).", lane);
            break;
          }

          if (xSemaphoreTake(semCapacity, 0) == pdTRUE) {
            S.carsInside++;
            changed = true;
            logf("entry ok (lane %d). carsinside=%d", lane, S.carsInside);
          } else {
            logf("entry refused: tunnel full.");
          }
          break;
        }

      case EV_EXIT_L1:
      case EV_EXIT_L2: {
          recomputeGates_locked();
          const int lane = (ev.type == EV_EXIT_L1) ? 1 : 2;

          if (!S.exitAllowed) {
            logf("exit blocked by operator (lane %d).", lane);
            break;
          }

          if (S.carsInside > 0) {
            S.carsInside--;
            xSemaphoreGive(semCapacity);
            changed = true;
            logf("exit ok (lane %d). carsinside=%d", lane, S.carsInside);
          } else {
            logf("exit ignored: carsinside already 0.");
          }
          break;
        }

      case EV_GAS_ON:
        S.gas = true;
        changed = true;
        logf("gas leak on");
        break;

      case EV_GAS_OFF:
        S.gas = false;
        changed = true;
        logf("gas leak off");
        break;

      case EV_SMOKE_ON:
        S.smoke = true;
        changed = true;
        logf("smoke on");
        break;

      case EV_SMOKE_OFF:
        S.smoke = false;
        changed = true;
        logf("smoke off");
        break;

      case EV_PANIC_ON:
        S.panic = true;
        changed = true;
        logf("panic on");
        break;

      case EV_PANIC_OFF:
        S.panic = false;
        changed = true;
        logf("panic off");
        break;


      case EV_OP_BLOCK_IN_ON:
        if (!S.opBlockIn) {
          S.opBlockIn = true;
          changed = true;
        }
        logf("operator: block entry on");
        break;

      case EV_OP_BLOCK_IN_OFF:
        if (S.opBlockIn) {
          S.opBlockIn = false;
          changed = true;
        }
        logf("operator: block entry off");
        break;

      case EV_OP_BLOCK_OUT_ON:
        if (!S.opBlockOut) {
          S.opBlockOut = true;
          changed = true;
        }
        logf("operator: block exit on");
        break;

      case EV_OP_BLOCK_OUT_OFF:
        if (S.opBlockOut) {
          S.opBlockOut = false;
          changed = true;
        }
        logf("operator: block exit off");
        break;

      case EV_STATUS_REQ:
        statusRequested = true;
        break;

      case EV_HELP:
        break;
    }

    if (changed) {
      recomputeGates_locked();
      applyOutputs_locked();
    }

    xSemaphoreGive(mtxState);

    if (changed || statusRequested) {
      printState();
    }

    if (ev.type == EV_HELP) {
      logf("commands:");
      logf("e1 entry lane 1, x1 exit lane 1");
      logf("e2 entry lane 2, x2 exit lane 2");
      logf("gas on/off, smoke on/off, panic on/off");
      logf("bin on/off operator block entry");
      logf("bout on/off operator block exit");
      logf("auto on/off traffic emulator");
      logf("rate <ms> emulator period");
      logf("burst <k> send k entry events");
      logf("status show state, help show commands");
    }
  }
}

// parseaza comenzi din serial
static void handleCommand(char* line) {
  while (*line == ' ' || *line == '\t') line++;
  if (*line == 0) return;

  for (char* p = line; *p; ++p) {
    if (*p >= 'A' && *p <= 'Z') *p = *p - 'A' + 'a';
  }

  if (strcmp(line, "e1") == 0) {
    enqueueEvent(EV_ENTRY_L1);
    return;
  }
  if (strcmp(line, "x1") == 0) {
    enqueueEvent(EV_EXIT_L1);
    return;
  }
  if (strcmp(line, "e2") == 0) {
    enqueueEvent(EV_ENTRY_L2);
    return;
  }
  if (strcmp(line, "x2") == 0) {
    enqueueEvent(EV_EXIT_L2);
    return;
  }

  if (strcmp(line, "status") == 0) {
    enqueueEvent(EV_STATUS_REQ);
    return;
  }
  if (strcmp(line, "help") == 0)   {
    enqueueEvent(EV_HELP);
    return;
  }

  char* a = strtok(line, " ");
  char* b = strtok(NULL, " ");
  if (!a) return;

  if (b) {
    if (strcmp(a, "auto") == 0) {
      const bool on = (strcmp(b, "on") == 0 || strcmp(b, "1") == 0);
      xSemaphoreTake(mtxState, portMAX_DELAY);
      const bool prev = S.emuOn;
      S.emuOn = on;
      xSemaphoreGive(mtxState);

      if (on != prev) {
        logf("auto %s", on ? "on" : "off");
        printState();
      } else {
        logf("auto already %s", on ? "on" : "off");
      }
      return;
    }

    if (strcmp(a, "rate") == 0) {
      const int r = atoi(b);
      if (r < 50 || r > 5000) {
        logf("rate invalid 50..5000");
        return;
      }

      xSemaphoreTake(mtxState, portMAX_DELAY);
      const uint16_t prev = S.emuRateMs;
      S.emuRateMs = (uint16_t)r;
      xSemaphoreGive(mtxState);

      if (prev != (uint16_t)r) {
        logf("auto rate set to %d ms", r);
        printState();
      } else {
        logf("auto rate unchanged");
      }
      return;
    }

    if (strcmp(a, "burst") == 0) {
      const int k = atoi(b);
      if (k <= 0 || k > 50) {
        logf("burst invalid 1..50");
        return;
      }

      logf("burst %d entries requested", k);
      for (int i = 0; i < k; i++) {
        xSemaphoreTake(mtxState, portMAX_DELAY);
        const bool lane = (S.emuLaneToggle++ & 1);
        xSemaphoreGive(mtxState);
        enqueueEvent(lane ? EV_ENTRY_L2 : EV_ENTRY_L1);
      }
      return;
    }

    const bool on = (strcmp(b, "on") == 0 || strcmp(b, "1") == 0);

    if (strcmp(a, "gas") == 0)   {
      enqueueEvent(on ? EV_GAS_ON   : EV_GAS_OFF);
      return;
    }
    if (strcmp(a, "smoke") == 0) {
      enqueueEvent(on ? EV_SMOKE_ON : EV_SMOKE_OFF);
      return;
    }
    if (strcmp(a, "panic") == 0) {
      enqueueEvent(on ? EV_PANIC_ON : EV_PANIC_OFF);
      return;
    }
    if (strcmp(a, "bin") == 0)   {
      enqueueEvent(on ? EV_OP_BLOCK_IN_ON  : EV_OP_BLOCK_IN_OFF);
      return;
    }
    if (strcmp(a, "bout") == 0)  {
      enqueueEvent(on ? EV_OP_BLOCK_OUT_ON : EV_OP_BLOCK_OUT_OFF);
      return;
    }
  }

  logf("unknown command. type help");
}

// task cli, citeste serial non blocking
static void TaskCLI(void*) {
  static char buf[80];
  uint8_t idx = 0;

  logf("cli ready. type help.");

  for (;;) {
    while (Serial.available() > 0) {
      const char c = (char)Serial.read();

      if (c == '\r' || c == '\n') {
        if (idx > 0) {
          buf[idx] = 0;
          handleCommand(buf);
          idx = 0;
        }
      } else {
        if (idx < sizeof(buf) - 1) buf[idx++] = c;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(CLI_POLL_MS));
  }
}

// task emulator, genereaza evenimente periodic, fara hardware
static void TaskSensorEmulator(void*) {
  for (;;) {
    xSemaphoreTake(mtxState, portMAX_DELAY);
    const bool on = S.emuOn;
    const uint16_t rate = S.emuRateMs;
    const bool entryAllowed = S.entryAllowed;
    const bool exitAllowed  = S.exitAllowed;
    const int cars = S.carsInside;
    const bool lane = (S.emuLaneToggle & 1);
    if (on) S.emuLaneToggle++;
    xSemaphoreGive(mtxState);

    if (on) {
      if (entryAllowed) {
        enqueueEvent(lane ? EV_ENTRY_L2 : EV_ENTRY_L1);
      } else {
        if (cars > 0 && exitAllowed) {
          enqueueEvent(lane ? EV_EXIT_L2 : EV_EXIT_L1);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(rate));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("boot ok");

  pinMode(PIN_GATE_IN_LED, OUTPUT);
  pinMode(PIN_GATE_OUT_LED, OUTPUT);
  pinMode(PIN_ALARM_LED, OUTPUT);

  S.N = N_DEFAULT;
  S.carsInside = 0;

  S.gas = false;
  S.smoke = false;
  S.panic = false;

  S.opBlockIn = false;
  S.opBlockOut = false;

  S.emuOn = false;
  S.emuRateMs = 800;
  S.emuLaneToggle = 0;

  qEvents = xQueueCreate(25, sizeof(Event));
  qLog    = xQueueCreate(30, sizeof(LogMsg));
  mtxState = xSemaphoreCreateMutex();
  semCapacity = xSemaphoreCreateCounting(S.N, S.N);

  if (qEvents == NULL) Serial.println("err: qevents null");
  if (qLog == NULL)    Serial.println("err: qlog null");
  if (mtxState == NULL) Serial.println("err: mtxstate null");
  if (semCapacity == NULL) Serial.println("err: semcapacity null");

  xSemaphoreTake(mtxState, portMAX_DELAY);
  recomputeGates_locked();
  applyOutputs_locked();
  xSemaphoreGive(mtxState);

  xTaskCreate(TaskLogger,         "logger",     256, NULL, 2, NULL);
  xTaskCreate(TaskCLI,            "cli",        256, NULL, 1, NULL);
  xTaskCreate(TaskController,     "controller", 256, NULL, 3, NULL);
  xTaskCreate(TaskSensorEmulator, "emu",        256, NULL, 1, NULL);
}

void loop() {
}
