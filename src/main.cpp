#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <time.h>
#include "secrets.h"

// ---------- CONFIGURAÇÃO ----------
const char* FUSO = "<-03>3";                     // horário de Brasília
const unsigned long INTERVALO_AGENDA_MS = 60000; // busca a agenda a cada 1 minuto
const unsigned long LIMITE_FIRMWARE_MS = 20000;  // trava absoluta, vale mais que o servidor
const unsigned long EPOCH_MINIMO = 1700000000;   // abaixo disso o relógio não sincronizou
const int MAX_HORARIOS = 20;
const int MAX_FILA = 8;

// ---------- COMPORTA (servo de posição) ----------
const uint8_t PINO_SERVO = 14;
const int ANGULO_FECHADO = 0;
const int ANGULO_ABERTO  = 90;

// Tempo que a comporta leva para abrir e para fechar. Nesse movimento a ração
// já está passando, então esse tempo é descontado do tempo calculado.
const unsigned long TEMPO_MOVIMENTO_MS = 300;

Servo servo;

void atuadorIniciar() {
  servo.setPeriodHertz(50);
  servo.attach(PINO_SERVO, 500, 2400);
  servo.write(ANGULO_FECHADO);   // sempre começa fechada
}

void atuadorLigar() {            // abre a comporta
  servo.write(ANGULO_ABERTO);
}

void atuadorDesligar() {         // fecha a comporta
  servo.write(ANGULO_FECHADO);
}

// ---------- DADOS ----------
struct Horario {
  String id;
  uint8_t hora = 0;
  uint8_t minuto = 0;
  uint8_t dias = 0;                // bit 0 = domingo ... bit 6 = sábado
  unsigned long duracaoMs = 0;
  unsigned long ultimoMinuto = 0;  // evita repetir no mesmo minuto
};

struct Pedido {
  String id;
  unsigned long duracaoMs = 0;
  uint32_t previsto = 0;
};

Horario horarios[MAX_HORARIOS];
int totalHorarios = 0;
unsigned long tempoMaximoMs = 15000;

Pedido fila[MAX_FILA];
int tamFila = 0;

bool dosando = false;
Pedido doseAtual;
unsigned long duracaoDose = 0;
unsigned long inicioDose = 0;
uint32_t iniciadoEm = 0;

bool relogioOk = false;
unsigned long ultimaAgenda = 0;
Preferences prefs;

// ---------- AGENDA ----------
bool carregarAgenda(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  if (!doc["horarios"].is<JsonArray>()) return false;

  Horario novos[MAX_HORARIOS];
  int n = 0;

  for (JsonObject h : doc["horarios"].as<JsonArray>()) {
    if (n >= MAX_HORARIOS) break;

    int hh = 0, mm = 0;
    const char* texto = h["hora"] | "";
    if (sscanf(texto, "%d:%d", &hh, &mm) != 2) continue;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) continue;

    unsigned long dur = h["duracaoMs"] | 0;
    if (dur == 0) continue;

    uint8_t mascara = 0;
    for (JsonVariant d : h["dias"].as<JsonArray>()) {
      int dia = d.as<int>();
      if (dia >= 0 && dia <= 6) mascara |= (1 << dia);
    }

    novos[n].id = String(h["id"] | "");
    novos[n].hora = hh;
    novos[n].minuto = mm;
    novos[n].dias = mascara;
    novos[n].duracaoMs = dur;

    // Mantém o "já executei neste minuto" ao atualizar a agenda
    for (int i = 0; i < totalHorarios; i++) {
      if (horarios[i].id == novos[n].id) {
        novos[n].ultimoMinuto = horarios[i].ultimoMinuto;
        break;
      }
    }
    n++;
  }

  for (int i = 0; i < n; i++) horarios[i] = novos[i];
  totalHorarios = n;
  tempoMaximoMs = doc["tempoMaximoMs"] | 15000;
  return true;
}

void listar() {
  time_t agora = time(nullptr);
  struct tm info;
  localtime_r(&agora, &info);
  char buf[32];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &info);

  Serial.printf("Hora atual: %s (relogio %s)\n", buf, relogioOk ? "ok" : "nao sincronizado");
  Serial.printf("Horarios: %d | tempo maximo: %lu ms\n", totalHorarios, tempoMaximoMs);
  for (int i = 0; i < totalHorarios; i++) {
    Serial.printf("  %02d:%02d  tempo %lu ms  dias(mascara) %d\n",
                  horarios[i].hora, horarios[i].minuto,
                  horarios[i].duracaoMs, horarios[i].dias);
  }
}

void atualizarAgenda() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sem Wi-Fi: usando a agenda guardada.");
    return;
  }

  HTTPClient http;
  http.begin(String(SERVIDOR) + "/api/dispositivo/agenda");
  http.addHeader("Authorization", String("Bearer ") + TOKEN);
  http.setTimeout(5000);

  int codigo = http.GET();
  if (codigo == 200) {
    String corpo = http.getString();
    if (carregarAgenda(corpo)) {
      if (corpo.length() < 3900) prefs.putString("agenda", corpo);  // guarda na memória
      Serial.printf("Agenda atualizada: %d horario(s)\n", totalHorarios);
    } else {
      Serial.println("Agenda recebida, mas invalida.");
    }
  } else {
    Serial.printf("Falha ao buscar a agenda (codigo %d)\n", codigo);
  }
  http.end();
}

// ---------- REGISTRO ----------
void enviarRegistro(const Pedido& p, uint32_t iniciado, unsigned long duracaoReal, const char* status) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sem Wi-Fi: registro nao enviado.");
    return;
  }

  JsonDocument doc;
  doc["horarioId"] = p.id;
  doc["previstoEm"] = p.previsto;
  doc["iniciadoEm"] = iniciado;
  doc["duracaoMs"] = duracaoReal;
  doc["status"] = status;

  String corpo;
  serializeJson(doc, corpo);

  HTTPClient http;
  http.begin(String(SERVIDOR) + "/api/dispositivo/registro");
  http.addHeader("Authorization", String("Bearer ") + TOKEN);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  int codigo = http.POST(corpo);
  Serial.printf("Registro enviado (codigo %d)\n", codigo);
  http.end();
}

// ---------- DOSAGEM ----------
void enfileirar(const Horario& h, uint32_t previsto) {
  if (tamFila >= MAX_FILA) {
    Serial.println("Fila cheia: dosagem ignorada!");
    return;
  }
  fila[tamFila].id = h.id;
  fila[tamFila].duracaoMs = h.duracaoMs;
  fila[tamFila].previsto = previsto;
  tamFila++;
}

void iniciarProximo() {
  if (dosando || tamFila == 0) return;

  doseAtual = fila[0];
  for (int i = 1; i < tamFila; i++) fila[i - 1] = fila[i];
  tamFila--;

  unsigned long dur = doseAtual.duracaoMs;
  if (dur > tempoMaximoMs) dur = tempoMaximoMs;
  if (dur > LIMITE_FIRMWARE_MS) dur = LIMITE_FIRMWARE_MS;

  // Desconta o tempo de abertura e fechamento da comporta
  duracaoDose = (dur > TEMPO_MOVIMENTO_MS + 50) ? dur - TEMPO_MOVIMENTO_MS : 50;

  iniciadoEm = (uint32_t)time(nullptr);
  inicioDose = millis();
  dosando = true;
  atuadorLigar();
  Serial.printf("Comporta ABERTA por %lu ms\n", duracaoDose);
}

void atualizarDose() {
  if (!dosando || millis() - inicioDose < duracaoDose) return;

  atuadorDesligar();
  dosando = false;
  unsigned long real = millis() - inicioDose;
  Serial.printf("Comporta FECHADA apos %lu ms\n", real);

  enviarRegistro(doseAtual, iniciadoEm, real, "OK");
}

// ---------- RELÓGIO E HORÁRIOS ----------
void verificarRelogio() {
  if (relogioOk || time(nullptr) < EPOCH_MINIMO) return;

  relogioOk = true;
  unsigned long minuto = time(nullptr) / 60;
  // Não dispara a dose do minuto em que a placa ligou (evita repetir após reiniciar)
  for (int i = 0; i < totalHorarios; i++) horarios[i].ultimoMinuto = minuto;

  Serial.println("Relogio sincronizado.");
  listar();
}

void verificarHorarios() {
  time_t agora = time(nullptr);
  struct tm info;
  localtime_r(&agora, &info);
  unsigned long minutoAtual = agora / 60;

  for (int i = 0; i < totalHorarios; i++) {
    Horario& h = horarios[i];
    if (h.ultimoMinuto == minutoAtual) continue;
    if (!(h.dias & (1 << info.tm_wday))) continue;
    if (h.hora != info.tm_hour || h.minuto != info.tm_min) continue;

    h.ultimoMinuto = minutoAtual;
    Serial.printf("Horario %02d:%02d chegou.\n", h.hora, h.minuto);
    enfileirar(h, (uint32_t)(minutoAtual * 60));
  }
}

// ---------- PROGRAMA ----------
void setup() {
  Serial.begin(115200);

  atuadorIniciar();  // sempre começa com a comporta fechada

  prefs.begin("comedouro", false);
  if (prefs.isKey("agenda")) {
    String salva = prefs.getString("agenda", "");
    if (salva.length() > 0 && carregarAgenda(salva)) {
      Serial.printf("Agenda guardada carregada: %d horario(s)\n", totalHorarios);
    }
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("Redes encontradas pela placa:");
  int achadas = WiFi.scanNetworks();
  if (achadas <= 0) Serial.println("  (nenhuma rede encontrada)");
  for (int i = 0; i < achadas; i++) {
    Serial.printf("  [%s] (%d dBm)\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
  }

  Serial.printf("Tentando a rede: [%s]\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  Serial.print("Conectando ao Wi-Fi");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("Status do Wi-Fi: %d\n", WiFi.status());

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Conectado. IP da placa: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Nao conectou (vai continuar tentando em segundo plano).");
  }

  configTzTime(FUSO, "pool.ntp.org", "time.google.com");
  atualizarAgenda();
  ultimaAgenda = millis();

  Serial.println("Comandos: l = listar | a = atualizar agenda | t = teste 2 s | c = calibrar 10 s");
}

void loop() {
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'l' || c == 'L') listar();
    if ((c == 'a' || c == 'A') && !dosando) atualizarAgenda();

    if ((c == 't' || c == 'T') && !dosando) {
      Serial.println("Teste: comporta aberta por 2 s...");
      atuadorLigar();
      delay(2000);
      atuadorDesligar();
      Serial.println("Comporta fechada.");
    }

    if ((c == 'c' || c == 'C') && !dosando) {
      Serial.println("Calibracao: comporta aberta por 10 s...");
      atuadorLigar();
      delay(10000);
      atuadorDesligar();
      Serial.println("Fechada. Meça a racao que caiu e divida por 10.");
    }
  }

  verificarRelogio();

  // Não busca a agenda durante uma dosagem (a requisição pode demorar)
  if (!dosando && millis() - ultimaAgenda >= INTERVALO_AGENDA_MS) {
    ultimaAgenda = millis();
    atualizarAgenda();
  }

  if (relogioOk) verificarHorarios();

  iniciarProximo();
  atualizarDose();
}