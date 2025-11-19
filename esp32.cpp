/* Sistema de Irrigação Inteligente com ESP32 (PlatformIO/VS Code)
   - OLED SH1106 128x64
   - Keypad 4x4
   - Sensor de Umidade do Solo
   - LED simulando bomba
   - Envio de dados para FastAPI local
   
*/
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Keypad.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>


// ==================== CONFIGURAÇÃO GERAL ====================

// WiFi
const char* ssid = "Lab111";
const char* password = "i9lab111";

// FASTAPI HOST: SUBSTITUA PELO IP REAL DO SEU PC

const char* FASTAPI_HOST = "192.168.0.103"; 
const int FASTAPI_PORT = 8000;

// Intervalo de envio para API: Inicialmente 10 segundos (10000 ms).
// Esta variável será alterada pelo usuário no menu de configuração (tecla 'C').
unsigned long API_SEND_INTERVAL = 10000; 

// CHAVE API (Corrigido para o valor do .env)
const char* API_SECRET_KEY = "minha-chave-secreta-esp32-123"; 

// Pinos
#define SOIL_PIN    36
#define LED_PIN     26
#define OLED_SDA    5
#define OLED_SCL    4

// OLED 
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Keypad 4x4
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {25, 16, 0, 2};
byte colPins[COLS] = {15, 13, 12, 14};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ==================== VARIÁVEIS DE ESTADO ====================

// Calibração do sensor (Valores de exemplo, recalibre se necessário)
int ADC_DRY = 3000;
int ADC_WET = 1200;

// Estado do sistema
float setpoint = 50.0;
float umidade = 0.0;
bool bombaLigada = false;

// Controle não-bloqueante
unsigned long lastSensorRead = 0;
unsigned long lastApiSend = 0;
const unsigned long SENSOR_INTERVAL = 2000;    // Lê sensor a cada 2s

// Menu e telas
enum Tela { TELA_PRINCIPAL, TELA_MENU_CONFIG, TELA_SETPOINT, TELA_CALIB_DRY, TELA_CALIB_WET, TELA_API_INTERVAL_CONFIG };
Tela telaAtual = TELA_PRINCIPAL;
String inputBuffer = "";

// Filtro de leitura (média móvel)
#define BUFFER_LEN 8
float readings[BUFFER_LEN];
int idx = 0;
bool bufferFilled = false;

// ==================== PROTÓTIPOS ====================
void atualizarTela();

// ==================== FUNÇÕES DO SENSOR ====================

// Conversão ADC para porcentagem
float adcToPct(int adc) {
  float pct = 100.0 * (ADC_DRY - adc) / float(ADC_DRY - ADC_WET);
  return constrain(pct, 0.0, 100.0);
}

// Leitura do sensor com filtro
float readSoilPct() {
  int raw = analogRead(SOIL_PIN);
  float pct = adcToPct(raw);
  
  readings[idx++] = pct;
  if (idx >= BUFFER_LEN) { 
    idx = 0; 
    bufferFilled = true; 
  }
  
  float sum = 0;
  int count = bufferFilled ? BUFFER_LEN : max(1, idx);
  for (int i = 0; i < count; i++) sum += readings[i];
  
  return sum / count;
}

// ==================== FUNÇÕES DA BOMBA ====================

void ligarBomba() {
  if (!bombaLigada) {
    digitalWrite(LED_PIN, HIGH);
    bombaLigada = true;
    Serial.println("BOMBA LIGADA");
  }
}

void desligarBomba() {
  if (bombaLigada) {
    digitalWrite(LED_PIN, LOW);
    bombaLigada = false;
    Serial.println("BOMBA DESLIGADA");
  }
}

// ==================== FUNÇÕES DE COMUNICAÇÃO (FastAPI) ====================

bool sendSoilData(float umidadePct) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi desconectado, não é possível enviar dados.");
        return false;
    }

    HTTPClient http;
    
    // 1. Constrói a URL usando o FASTAPI_HOST
    String url = "http://" + String(FASTAPI_HOST) + ":" + String(FASTAPI_PORT) + "/api/umidade/registrar"; 
    
    // 2. Payload JSON
    String jsonPayload = "{\"umidade\": " + String(umidadePct, 2) + "}";

    // 3. Inicia a requisição
    http.begin(url);
    
    // 4. Configuração dos Headers
    http.setReuse(false);
    http.addHeader("Content-Type", "application/json");
    
    // Adiciona o header de autenticação
    http.addHeader("X-API-Key", String(API_SECRET_KEY)); 

    int code = http.POST(jsonPayload);
    
    if (code > 0) {
        if (code == 200 || code == 201) {
            Serial.printf("Dados enviados com sucesso! Code: %d\n", code);
            http.end();
            return true;
        } else {
            Serial.printf("Erro ao enviar dados para a API. Code: %d\n", code);
            Serial.println("Resposta do Servidor:");
            Serial.println(http.getString()); 
            http.end();
            return false;
        }
    } else {
        // Alerta o usuário para verificar o servidor/IP.
        Serial.printf("ERRO FATAL HTTP CLIENT: Código: %d. Falha na conexão ou envio. (Verifique FASTAPI_HOST/Porta/Firewall)\n", code);
        http.end();
        return false;
    }
}

// ==================== INTERFACE OLED ====================

void drawTelaPrincipal() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // Título
  display.setCursor(0, 2);
  display.print("IRRIGACAO ESP32");
  
  // WiFi indicator
  if (WiFi.status() == WL_CONNECTED) {
    display.setCursor(115, 2);
    display.print("W");
  }
  
  // Barra de umidade
  int barX = 0, barY = 16, barW = 98, barH = 12;
  int fill = map(umidade, 0, 100, 0, barW);
  
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  display.fillRect(barX + 1, barY + 1, max(0, fill - 2), barH - 2, SSD1306_WHITE);
  
  // Valor da umidade
  display.setCursor(barW + 9, barY + 3);
  display.print(umidade, 0);
  display.print("%");
  
  // Setpoint
  display.setCursor(0, 31);
  display.print("Alvo: ");
  display.print(setpoint, 0);
  display.print("%");
  
  // Status da bomba
  display.setCursor(0, 43);
  display.print(bombaLigada ? "Bomba: LIGADA" : "Bomba: DESLIG");
  
  // Ajuda
  display.setCursor(0, 55);
  display.print("*=Menu Config");
  
  display.display();
}

void drawTelaMenuConfig() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(0, 2);
  display.print("MENU CONFIGURACAO");
  
  display.setCursor(0, 18);
  display.print("A: Calibrar Sensor");
  
  display.setCursor(0, 29);
  display.print("B: Configurar Alvo");
  
  display.setCursor(0, 40);
  display.print("C: API Update(");
  display.print(API_SEND_INTERVAL / 1000);
  display.print(" seg)");
  
  display.setCursor(0, 51);
  display.print("*: Voltar Principal");
  
  display.display();
}

void drawTelaSetpoint() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(4, 2);
  display.print("CONFIGURAR ALVO");
  
  display.setCursor(4, 18);
  display.print("Alvo Atual: ");
  display.print(setpoint, 0);
  display.print("%");
  
  display.setCursor(0, 34);
  display.print("Digite 0-100: ");
  display.print(inputBuffer);
  display.print("_");
    
  
  display.setCursor(4, 57);
  display.print("#=OK *=Voltar");
  
  display.display();
}

void drawTelaApiIntervalConfig() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(4, 2);
  display.print("CONFIG. INTERVALO");
  
  display.setCursor(4, 18);
  display.print("Atual: ");
  display.print(API_SEND_INTERVAL / 1000);
  display.print(" seg");
  
  display.setCursor(4, 34);
  display.print("Novo (seg): ");
  display.print(inputBuffer);
  display.print("_");
    
  
  display.setCursor(4, 57);
  display.print("#=OK *=Voltar");
  
  display.display();
}

void drawTelaCalibDry() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(4, 2);
  display.print("CALIBRACAO");
  
  display.setCursor(4, 18);
  display.print("Sensor no AR SECO");
  
  display.setCursor(4, 34);
  display.print("Pressione #");
  
  display.setCursor(4, 50);
  int rawAdc = analogRead(SOIL_PIN);
  display.print("ADC: ");
  display.print(rawAdc);
  
  display.display();
}

void drawTelaCalibWet() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(4, 2);
  display.print("CALIBRACAO");
  
  display.setCursor(4, 18);
  display.print("Sensor na AGUA");
  
  display.setCursor(4, 34);
  display.print("Pressione #");
  
  display.setCursor(4, 50);
  int rawAdc = analogRead(SOIL_PIN);
  display.print("ADC: ");
  display.print(rawAdc);
  
  display.display();
}

void atualizarTela() {
  switch (telaAtual) {
    case TELA_PRINCIPAL:             drawTelaPrincipal(); break;
    case TELA_MENU_CONFIG:           drawTelaMenuConfig(); break; 
    case TELA_SETPOINT:              drawTelaSetpoint(); break;
    case TELA_CALIB_DRY:             drawTelaCalibDry(); break;
    case TELA_CALIB_WET:             drawTelaCalibWet(); break;
    case TELA_API_INTERVAL_CONFIG:   drawTelaApiIntervalConfig(); break; 
  }
}

// ==================== KEYPAD ====================

void handleKeypad() {
  char k = keypad.getKey();
  if (!k) return;
  
  Serial.printf("Tecla: %c | Tela: %d\n", k, telaAtual);
  
  switch (telaAtual) {
    
    case TELA_PRINCIPAL:
      if (k == '*') {
        telaAtual = TELA_MENU_CONFIG;
      }
      break;

    case TELA_MENU_CONFIG:
      if (k == 'A') {
        telaAtual = TELA_CALIB_DRY;
      } else if (k == 'B') {
        telaAtual = TELA_SETPOINT;
        inputBuffer = "";
      } else if (k == 'C') {
        telaAtual = TELA_API_INTERVAL_CONFIG;
        inputBuffer = "";
      } else if (k == '*') {
        telaAtual = TELA_PRINCIPAL;
      }
      break;
      
    case TELA_SETPOINT:
      if (k == '#') {
        float val = inputBuffer.toFloat();
        if (val >= 0 && val <= 100) {
          setpoint = val;
          Serial.printf("Setpoint alterado: %.0f%%\n", setpoint);
        }
        inputBuffer = "";
        telaAtual = TELA_MENU_CONFIG; // Volta para o Menu
      }
      else if (k == '*') {
        inputBuffer = "";
        telaAtual = TELA_MENU_CONFIG;
      }
      else if (k >= '0' && k <= '9') {
        if (inputBuffer.length() < 3) {
          inputBuffer += k;
        }
      }
      break;
      
    // ALTERADO: CONFIGURAÇÃO DO INTERVALO API EM SEGUNDOS
    case TELA_API_INTERVAL_CONFIG:
      if (k == '#') {
        // Confirma o valor (em segundos)
        unsigned long val_sec = inputBuffer.toInt();
        if (val_sec >= 1) { // Garante que o intervalo mínimo é 1 segundo
          API_SEND_INTERVAL = val_sec * 1000; // Converte Segundos para Milissegundos
          Serial.printf("Intervalo API alterado: %lu segundos (%lu ms)\n", val_sec, API_SEND_INTERVAL);
        } else {
             Serial.println("ERRO: Intervalo API deve ser no mínimo 1 segundo.");
        }
        inputBuffer = "";
        telaAtual = TELA_MENU_CONFIG; // Volta para o Menu
      }
      else if (k == '*') {
        inputBuffer = "";
        telaAtual = TELA_MENU_CONFIG;
      }
      else if (k >= '0' && k <= '9') {
        // Máximo 4 dígitos (até 9999 segundos)
        if (inputBuffer.length() < 4) {
          inputBuffer += k;
        }
      }
      break;

    case TELA_CALIB_DRY:
      if (k == '#') {
        ADC_DRY = analogRead(SOIL_PIN);
        Serial.printf("Calibrado SECO: %d\n", ADC_DRY);
        telaAtual = TELA_CALIB_WET;
      }
      else if (k == '*') {
         telaAtual = TELA_MENU_CONFIG;
      }
      break;
      
    case TELA_CALIB_WET:
      if (k == '#') {
        ADC_WET = analogRead(SOIL_PIN);
        Serial.printf("Calibrado MOLHADO: %d\n", ADC_WET);
        telaAtual = TELA_MENU_CONFIG; // Volta para o Menu após calibração
      }
      else if (k == '*') {
         telaAtual = TELA_MENU_CONFIG;
      }
      break;
  }
  
  atualizarTela();
}

// ==================== LÓGICA DE IRRIGAÇÃO (SIMPLIFICADA) ====================

void controlIrrigation() {
  // Limites de umidade definem a ação
  
  // LIGA a bomba se a umidade estiver ABAIXO do setpoint
  if (umidade < setpoint) {
    ligarBomba();
  }
  // DESLIGA a bomba se a umidade estiver ACIMA ou IGUAL ao setpoint
  else if (umidade >= setpoint) {
    desligarBomba();
  }
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Sistema de Irrigacao ESP32");
  
  // LED (bomba)
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // I2C para OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  
  // OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Falha ao iniciar display SSD1306"));
    for(;;);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 2);
  display.print("Iniciando...");
  display.display();
  
  // WiFi
  Serial.print("Conectando WiFi");
  WiFi.begin(ssid, password);
  
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi nao conectado.");
  }
  
  // Tela principal
  telaAtual = TELA_PRINCIPAL;
  atualizarTela();
  
  Serial.println("Sistema pronto!");
  Serial.println("Teclas: * = Menu Config");
}
// ==================== LOOP ====================

void loop() {
  unsigned long now = millis();
  
  // Leitura do sensor (a cada 2s)
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    umidade = readSoilPct();
    lastSensorRead = now;
    
    // Atualiza tela principal
    if (telaAtual == TELA_PRINCIPAL) {
      atualizarTela();
    }
  }
  
  // Envio de Dados para o FastAPI (usa API_SEND_INTERVAL, que agora é dinâmico)
  if (now - lastApiSend >= API_SEND_INTERVAL) {
      if (WiFi.status() == WL_CONNECTED) {
          sendSoilData(umidade); 
      }
      lastApiSend = now;
  }
  
  // Teclado (sempre verifica)
  handleKeypad();
  
  // Lógica de irrigação (sempre executa)
  controlIrrigation();
  
  // Pequeno delay para não sobrecarregar
  delay(50);
}