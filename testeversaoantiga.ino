#include "esp_camera.h" // Biblioteca oficial para controle da câmera no ESP32
#include <WiFi.h>         // Biblioteca para conexão WiFi
#include <HTTPClient.h>   // Biblioteca para realizar requisições HTTP (cliente)

// Variável global para controle de acesso (modificada pelo app_httpd.cpp)
// true = acesso liberado (rosto reconhecido), false = acesso negado
boolean autorizacao_acesso = false;

//
// AVISO!!! Certifique-se de ter selecionado o módulo ESP32 Wrover,
//          ou outra placa que tenha PSRAM habilitada (necessário para resoluções maiores e reconhecimento facial)
//

// Seleção do modelo da câmera (Descomente apenas o modelo que você está usando)
//#define CAMERA_MODEL_WROVER_KIT
//#define CAMERA_MODEL_ESP_EYE
//#define CAMERA_MODEL_M5STACK_PSRAM
//#define CAMERA_MODEL_M5STACK_WIDE
#define CAMERA_MODEL_AI_THINKER // Modelo comum: ESP32-CAM AI-Thinker

#include "camera_pins.h" // Arquivo auxiliar com a definição dos pinos para cada modelo de câmera

// Credenciais da rede WiFi
const char* ssid = "Yan";
const char* password = "yanduda123";

// Definição dos pinos dos LEDs
#define LED_VERDE 2      // LED indicador de sucesso (GPIO 2)
#define LED_VERMELHO 14  // LED indicador de falha/erro (GPIO 14)
#define LED_FLASH 4      // LED de flash (branco forte) da ESP32-CAM (GPIO 4)

// Declaração da função que inicia o servidor da câmera (definida em app_httpd.cpp)
void startCameraServer();

void setup() {
  // --- Configuração inicial do LED de Flash (Sinal de Vida) ---
  pinMode(LED_FLASH, OUTPUT);      // Configura o pino do flash como saída
  digitalWrite(LED_FLASH, HIGH);   // Acende o flash com intensidade máxima
  delay(2000);                     // Espera 2 segundos com o flash ligado para indicar boot
  digitalWrite(LED_FLASH, LOW);    // Apaga o flash

  // Inicialização da comunicação Serial para debug
  Serial.begin(115200);
  Serial.setDebugOutput(true); // Habilita mensagens de debug da biblioteca WiFi/Camera
  Serial.println();

  // Configuração da estrutura da câmera
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;       // Frequência do clock XCLK (20MHz)
  config.pixel_format = PIXFORMAT_JPEG; // Formato de pixel (JPEG para streaming web)
  
  // Inicialização com especificações altas se houver PSRAM
  if(psramFound()){
    config.frame_size = FRAMESIZE_UXGA; // Resolução 1600x1200
    config.jpeg_quality = 10;           // Qualidade JPEG (0-63, menor é melhor)
    // IMPORTANTE: fb_count = 1 para evitar corrupção de memória em placas com 2MB de PSRAM
    config.fb_count = 1;                // Número de framebuffers
  } else {
    // Se não houver PSRAM, limita a resolução para economizar memória
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // Inicializa a câmera com as configurações definidas
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return; // Se falhar, encerra o setup (trava o código aqui)
  }

  // Ajustes finos nos sensores da câmera
  sensor_t * s = esp_camera_sensor_get();
  // Os sensores iniciais podem estar invertidos verticalmente e com cores saturadas
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);       // Inverte verticalmente de volta
    s->set_brightness(s, 1);  // Aumenta um pouco o brilho
    s->set_saturation(s, -2); // Diminui a saturação
  }
  // Reduz o tamanho do frame inicial para aumentar a taxa de quadros (fps)
  s->set_framesize(s, FRAMESIZE_QVGA); // Resolução 320x240

#if defined(CAMERA_MODEL_M5STACK_WIDE)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

  // Conexão WiFi
  WiFi.begin(ssid, password);

  // Aguarda a conexão ser estabelecida
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  // Inicia o servidor web da câmera (função em app_httpd.cpp)
  startCameraServer();

  // Exibe o IP para acesso no monitor serial
  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
  
  // Configura os pinos dos LEDs de status como saída
  pinMode (LED_VERDE, OUTPUT);
  pinMode (LED_VERMELHO, OUTPUT);
}

void loop() {
  // --- Máquina de Estados para Controle de Acesso ---
  
  // Verifica a variável global de autorização (atualizada pelo reconhecimento facial)
  if(autorizacao_acesso == true)
  {
    Serial.print("Acesso Liberado! \n");
    digitalWrite(LED_VERDE, HIGH);   // Acende LED Verde
    digitalWrite(LED_VERMELHO, LOW); // Apaga LED Vermelho
    delay(5000);                     // Mantém liberado por 5 segundos
    autorizacao_acesso = false;      // Reseta a autorização para o estado padrão
  }
  else
  {
    // Se não autorizado (ou aguardando reconhecimento)
    Serial.print("Acesso Negado! \n");
    digitalWrite(LED_VERMELHO, HIGH); // Mantém LED Vermelho aceso (estado padrão: bloqueado)
    digitalWrite(LED_VERDE, LOW);     // Mantém LED Verde apagado
  }
  
  // Pequeno delay para não saturar o processador, mas manter a resposta rápida (10Hz)
  delay(100); 
}
