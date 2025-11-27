// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Inclusão das bibliotecas necessárias para o funcionamento do servidor web e da câmera
#include "esp_http_server.h" // Biblioteca para criar o servidor HTTP no ESP32
#include "esp_timer.h"       // Biblioteca para temporização e contagem de tempo
#include "esp_camera.h"      // Biblioteca principal para controle da câmera OV2640/OV3660
#include "img_converters.h"  // Biblioteca para conversão de formatos de imagem (ex: RGB para JPEG)
#include "camera_index.h"    // Arquivo de cabeçalho contendo o HTML da página web (geralmente comprimido em gzip)
#include "Arduino.h"         // Biblioteca base do Arduino para ESP32

// Inclusão de bibliotecas para processamento de imagem e reconhecimento facial
#include "fb_gfx.h"          // Biblioteca gráfica para desenhar no framebuffer (caixas, texto)
#include "fd_forward.h"      // Biblioteca de detecção facial (Face Detection)
#include "fr_forward.h"      // Biblioteca de reconhecimento facial (Face Recognition)
#include <HTTPClient.h>      // Biblioteca para fazer requisições HTTP (POST, GET) para outros servidores

// Definições de constantes para o reconhecimento facial
#define ENROLL_CONFIRM_TIMES 5 // Número de vezes que o rosto deve ser detectado para confirmar o cadastro
#define FACE_ID_SAVE_NUMBER 7  // Número máximo de IDs de rosto que podem ser salvos

// Definições de cores para desenho na imagem (formato ARGB ou similar)
#define FACE_COLOR_WHITE  0x00FFFFFF
#define FACE_COLOR_BLACK  0x00000000
#define FACE_COLOR_RED    0x000000FF
#define FACE_COLOR_GREEN  0x0000FF00
#define FACE_COLOR_BLUE   0x00FF0000
#define FACE_COLOR_YELLOW (FACE_COLOR_RED | FACE_COLOR_GREEN)
#define FACE_COLOR_CYAN   (FACE_COLOR_BLUE | FACE_COLOR_GREEN)
#define FACE_COLOR_PURPLE (FACE_COLOR_BLUE | FACE_COLOR_RED)

// Estrutura para o filtro de média móvel (usado para suavizar a taxa de quadros - FPS)
typedef struct {
        size_t size; // Tamanho da amostra para o filtro (número de valores armazenados)
        size_t index; // Índice atual no array de valores
        size_t count; // Contagem atual de valores inseridos
        int sum;      // Soma atual dos valores (para cálculo rápido da média)
        int * values; // Ponteiro para o array que armazena os valores
} ra_filter_t;

// Estrutura para gerenciar o envio de imagens JPEG em pedaços (chunks)
typedef struct {
        httpd_req_t *req; // Ponteiro para a requisição HTTP atual
        size_t len;       // Tamanho total dos dados enviados
} jpg_chunking_t;

// Definições para o stream de vídeo MJPEG (Multipart JPEG)
#define PART_BOUNDARY "123456789000000000000987654321" // Delimitador único para separar os frames no stream
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY; // Cabeçalho Content-Type para stream MJPEG
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n"; // String delimitadora entre frames
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"; // Cabeçalho de cada parte (frame JPEG)

// Variáveis globais
static ra_filter_t ra_filter; // Instância do filtro de média móvel
httpd_handle_t stream_httpd = NULL; // Handle para o servidor HTTP do stream de vídeo
httpd_handle_t camera_httpd = NULL; // Handle para o servidor HTTP de controle e captura

// Configurações e variáveis de estado para detecção/reconhecimento facial
static mtmn_config_t mtmn_config = {0}; // Configuração da rede neural de detecção facial (MTMN)
static int8_t detection_enabled = 0;    // Flag para habilitar/desabilitar detecção facial
static int8_t recognition_enabled = 0;  // Flag para habilitar/desabilitar reconhecimento facial
static int8_t is_enrolling = 0;         // Flag para indicar se está no modo de cadastro de rosto
static face_id_list id_list = {0};      // Lista de IDs de rostos cadastrados localmente (na memória RAM do ESP32)

// Variável externa (definida no arquivo .ino) para controlar o acesso (LEDs)
extern boolean autorizacao_acesso;

// Função para inicializar o filtro de média móvel
static ra_filter_t * ra_filter_init(ra_filter_t * filter, size_t sample_size){
    // Limpa a estrutura do filtro
    memset(filter, 0, sizeof(ra_filter_t));

    // Aloca memória para o array de valores
    filter->values = (int *)malloc(sample_size * sizeof(int));
    if(!filter->values){
        return NULL; // Retorna NULL se falhar a alocação
    }
    // Inicializa o array com zeros
    memset(filter->values, 0, sample_size * sizeof(int));

    filter->size = sample_size; // Define o tamanho do filtro
    return filter;
}

// Função para adicionar um valor ao filtro e obter a média atual
static int ra_filter_run(ra_filter_t * filter, int value){
    if(!filter->values){
        return value; // Se não houver buffer, retorna o valor bruto
    }
    // Subtrai o valor antigo que será sobrescrito da soma
    filter->sum -= filter->values[filter->index];
    // Armazena o novo valor na posição atual
    filter->values[filter->index] = value;
    // Adiciona o novo valor à soma
    filter->sum += filter->values[filter->index];
    // Avança o índice circularmente
    filter->index++;
    filter->index = filter->index % filter->size;
    // Incrementa a contagem até atingir o tamanho total
    if (filter->count < filter->size) {
        filter->count++;
    }
    // Retorna a média (soma / contagem)
    return filter->sum / filter->count;
}

// Função auxiliar para desenhar texto na imagem (usada para debug/status)
static void rgb_print(dl_matrix3du_t *image_matrix, uint32_t color, const char * str){
    fb_data_t fb;
    fb.width = image_matrix->w;
    fb.height = image_matrix->h;
    fb.data = image_matrix->item;
    fb.bytes_per_pixel = 3;
    fb.format = FB_BGR888;
    // Desenha o texto centralizado na parte superior
    fb_gfx_print(&fb, (fb.width - (strlen(str) * 14)) / 2, 10, color, str);
}

// Função auxiliar tipo printf para desenhar texto formatado na imagem
static int rgb_printf(dl_matrix3du_t *image_matrix, uint32_t color, const char *format, ...){
    char loc_buf[64];
    char * temp = loc_buf;
    int len;
    va_list arg;
    va_list copy;
    va_start(arg, format);
    va_copy(copy, arg);
    // Formata a string
    len = vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
    va_end(copy);
    // Se o buffer local for pequeno, aloca um maior
    if(len >= sizeof(loc_buf)){
        temp = (char*)malloc(len+1);
        if(temp == NULL) {
            return 0;
        }
    }
    vsnprintf(temp, len+1, format, arg);
    va_end(arg);
    // Desenha a string formatada
    rgb_print(image_matrix, color, temp);
    if(len > 64){
        free(temp); // Libera memória se foi alocada dinamicamente
    }
    return len;
}

// Função para desenhar as caixas ao redor dos rostos detectados
static void draw_face_boxes(dl_matrix3du_t *image_matrix, box_array_t *boxes, int face_id){
    int x, y, w, h, i;
    uint32_t color = FACE_COLOR_YELLOW; // Cor padrão: Amarelo (detectado, mas não reconhecido/verificado)
    if(face_id < 0){
        color = FACE_COLOR_RED; // Vermelho: Rosto desconhecido ou erro
    } else if(face_id > 0){
        color = FACE_COLOR_GREEN; // Verde: Rosto reconhecido/autorizado
    }
    fb_data_t fb;
    fb.width = image_matrix->w;
    fb.height = image_matrix->h;
    fb.data = image_matrix->item;
    fb.bytes_per_pixel = 3;
    fb.format = FB_BGR888;
    // Itera sobre todas as caixas de rostos detectados
    for (i = 0; i < boxes->len; i++){
        // rectangle box
        x = (int)boxes->box[i].box_p[0];
        y = (int)boxes->box[i].box_p[1];
        w = (int)boxes->box[i].box_p[2] - x + 1;
        h = (int)boxes->box[i].box_p[3] - y + 1;
        // Desenha as linhas da caixa
        fb_gfx_drawFastHLine(&fb, x, y, w, color);
        fb_gfx_drawFastHLine(&fb, x, y+h-1, w, color);
        fb_gfx_drawFastVLine(&fb, x, y, h, color);
        fb_gfx_drawFastVLine(&fb, x+w-1, y, h, color);
#if 0
        // Código comentado para desenhar landmarks (pontos chave do rosto: olhos, nariz, boca)
        // landmark
        int x0, y0, j;
        for (j = 0; j < 10; j+=2) {
            x0 = (int)boxes->landmark[i].landmark_p[j];
            y0 = (int)boxes->landmark[i].landmark_p[j+1];
            fb_gfx_fillRect(&fb, x0, y0, 3, 3, color);
        }
#endif
    }
}

// Função principal para gerenciar o reconhecimento facial e comunicação com o servidor externo
static int run_face_recognition(dl_matrix3du_t *image_matrix, box_array_t *net_boxes) {
  dl_matrix3du_t *aligned_face = NULL;
  int matched_id = -1; 

  // Aloca memória para o rosto alinhado (56x56 pixels)
  aligned_face = dl_matrix3du_alloc(1, FACE_WIDTH, FACE_HEIGHT, 3);
  if (!aligned_face) {
    Serial.println("Could not allocate face recognition buffer");
    return -1;
  }

  // 1. ALINHAMENTO (Pré-processamento da IA)
  if (align_face(net_boxes, image_matrix, aligned_face) == ESP_OK) {
    
    // Prepara os dados do embedding para envio (bytes brutos da imagem alinhada)
    uint8_t *face_template_data = aligned_face->item;
    size_t face_template_size = aligned_face->w * aligned_face->h * aligned_face->c;

    // 2. LÓGICA DE CADASTRO (Se o botão "Enroll" foi clicado)
    if (is_enrolling == 1) {
      Serial.println("A registar um novo rosto no servidor...");
      
      HTTPClient http;
      // Conecta ao endpoint de registro do servidor Python
      // NOTA: O IP deve ser atualizado conforme a rede
      http.begin("http://10.214.22.211:5000/registar_rosto"); 

      http.addHeader("Content-Type", "application/octet-stream");

      // Envia os dados via POST
      int httpCode = http.POST(face_template_data, face_template_size);

      if (httpCode == HTTP_CODE_OK) {
        Serial.printf("Rosto registado com sucesso!\n");
        enroll_face(&id_list, aligned_face); // Mantém contagem local (visual)
      } else {
        Serial.printf("Erro ao registar rosto. Código: %d\n", httpCode);
      }
      http.end();
      is_enrolling = 0; // Desativa modo de cadastro

    // 3. LÓGICA DE RECONHECIMENTO (Se "Face Recognition" estiver ativo)
    } else if (recognition_enabled) {
      Serial.println("A verificar rosto no servidor...");
      
      HTTPClient http;
      // Conecta ao endpoint de reconhecimento
      http.begin("http://10.214.22.211:5000/reconhecer_rosto"); 
      http.addHeader("Content-Type", "application/octet-stream");

      // Envia os dados para comparação
      int httpCode = http.POST(face_template_data, face_template_size);

      if (httpCode == HTTP_CODE_OK) {
        String resposta_servidor = http.getString(); // Recebe o nome ou "Desconhecido"
        Serial.printf("Resposta do servidor: %s\n", resposta_servidor.c_str());
        
        if (resposta_servidor != "Rosto Desconhecido") {
          autorizacao_acesso = true;  // SUCESSO! Ativa LED Verde
          matched_id = 1; // Quadrado Verde
          rgb_printf(image_matrix, FACE_COLOR_GREEN, "%s", resposta_servidor.c_str()); // Escreve o nome
        } else {
          autorizacao_acesso = false; // FALHA! LED Vermelho
          rgb_print(image_matrix, FACE_COLOR_RED, "Desconhecido");
        }
      } else {
        Serial.printf("Erro na verificação. Código: %d\n", httpCode);
        autorizacao_acesso = false;
        rgb_print(image_matrix, FACE_COLOR_RED, "Erro Servidor");
      }
      http.end();
    }
  } else {
    Serial.println("Face Not Aligned");
    autorizacao_acesso = false;
  }

  dl_matrix3du_free(aligned_face);
  return matched_id;
}


// Função de callback para enviar chunks de imagem JPEG via HTTP
static size_t jpg_encode_stream(void * arg, size_t index, const void* data, size_t len){
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if(!index){
        j->len = 0; // Reinicia o contador no primeiro chunk
    }
    // Envia o chunk atual para o cliente HTTP
    if(httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK){
        return 0; // Erro no envio
    }
    j->len += len; // Incrementa o total enviado
    return len;
}

// Handler para a rota /capture (tira uma foto estática)
static esp_err_t capture_handler(httpd_req_t *req){
    // ... (Código padrão de captura mantido) ...
    // (Este handler é útil se você quiser tirar uma foto única)
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    int64_t fr_start = esp_timer_get_time();

    fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    size_t out_len, out_width, out_height;
    uint8_t * out_buf;
    bool s;
    bool detected = false;
    int face_id = 0;
    
    if(!detection_enabled || fb->width > 400){
        size_t fb_len = 0;
        if(fb->format == PIXFORMAT_JPEG){
            fb_len = fb->len;
            res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
        } else {
            jpg_chunking_t jchunk = {req, 0};
            res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk)?ESP_OK:ESP_FAIL;
            httpd_resp_send_chunk(req, NULL, 0);
            fb_len = jchunk.len;
        }
        esp_camera_fb_return(fb);
        int64_t fr_end = esp_timer_get_time();
        Serial.printf("JPG: %uB %ums\n", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start)/1000));
        return res;
    }

    dl_matrix3du_t *image_matrix = dl_matrix3du_alloc(1, fb->width, fb->height, 3);
    if (!image_matrix) {
        esp_camera_fb_return(fb);
        Serial.println("dl_matrix3du_alloc failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    out_buf = image_matrix->item;
    out_len = fb->width * fb->height * 3;
    out_width = fb->width;
    out_height = fb->height;

    s = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
    esp_camera_fb_return(fb);
    if(!s){
        dl_matrix3du_free(image_matrix);
        Serial.println("to rgb888 failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    box_array_t *net_boxes = face_detect(image_matrix, &mtmn_config);

    if (net_boxes){
        detected = true;
        if(recognition_enabled){
            face_id = run_face_recognition(image_matrix, net_boxes);
        }
        draw_face_boxes(image_matrix, net_boxes, face_id);
        free(net_boxes->score);
        free(net_boxes->box);
        free(net_boxes->landmark);
        free(net_boxes);
    }

    jpg_chunking_t jchunk = {req, 0};
    s = fmt2jpg_cb(out_buf, out_len, out_width, out_height, PIXFORMAT_RGB888, 90, jpg_encode_stream, &jchunk);
    dl_matrix3du_free(image_matrix);
    if(!s){
        Serial.println("JPEG compression failed");
        return ESP_FAIL;
    }

    int64_t fr_end = esp_timer_get_time();
    Serial.printf("FACE: %uB %ums %s%d\n", (uint32_t)(jchunk.len), (uint32_t)((fr_end - fr_start)/1000), detected?"DETECTED ":"", face_id);
    return res;
}

// Handler para a rota /stream (stream de vídeo MJPEG)
static esp_err_t stream_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char * part_buf[64];
    dl_matrix3du_t *image_matrix = NULL;
    bool detected = false;
    int face_id = 0;
    int64_t fr_start = 0;
    int64_t fr_ready = 0;
    int64_t fr_face = 0;
    int64_t fr_recognize = 0;
    int64_t fr_encode = 0;

    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while(true){
        detected = false;
        face_id = 0;
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
        } else {
            fr_start = esp_timer_get_time();
            fr_ready = fr_start;
            fr_face = fr_start;
            fr_encode = fr_start;
            fr_recognize = fr_start;
            
            if(!detection_enabled || fb->width > 400){
                if(fb->format != PIXFORMAT_JPEG){
                    bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                    esp_camera_fb_return(fb);
                    fb = NULL;
                    if(!jpeg_converted){
                        Serial.println("JPEG compression failed");
                        res = ESP_FAIL;
                    }
                } else {
                    _jpg_buf_len = fb->len;
                    _jpg_buf = fb->buf;
                }
            } else {
                // --- INÍCIO DO PROCESSAMENTO DE IA NO STREAM ---
                image_matrix = dl_matrix3du_alloc(1, fb->width, fb->height, 3);

                if (!image_matrix) {
                    Serial.println("dl_matrix3du_alloc failed");
                    res = ESP_FAIL;
                } else {
                    if(!fmt2rgb888(fb->buf, fb->len, fb->format, image_matrix->item)){
                        Serial.println("fmt2rgb888 failed");
                        res = ESP_FAIL;
                    } else {
                        fr_ready = esp_timer_get_time();
                        box_array_t *net_boxes = NULL;
                        
                        // 1. Deteção Facial
                        if(detection_enabled){
                            net_boxes = face_detect(image_matrix, &mtmn_config);
                        }
                        fr_face = esp_timer_get_time();
                        fr_recognize = fr_face;
                        
                        if (net_boxes || fb->format != PIXFORMAT_JPEG){
                            if(net_boxes){
                                detected = true;
                                // 2. Reconhecimento Facial (Chama a nossa função de rede)
                                if(recognition_enabled){
                                    face_id = run_face_recognition(image_matrix, net_boxes);
                                }
                                fr_recognize = esp_timer_get_time();
                                // 3. Desenha o resultado na imagem
                                draw_face_boxes(image_matrix, net_boxes, face_id);
                                free(net_boxes->score);
                                free(net_boxes->box);
                                free(net_boxes->landmark);
                                free(net_boxes);
                            }
                            // Converte de volta para JPEG para envio
                            if(!fmt2jpg(image_matrix->item, fb->width*fb->height*3, fb->width, fb->height, PIXFORMAT_RGB888, 90, &_jpg_buf, &_jpg_buf_len)){
                                Serial.println("fmt2jpg failed");
                                res = ESP_FAIL;
                            }
                            esp_camera_fb_return(fb);
                            fb = NULL;
                        } else {
                            _jpg_buf = fb->buf;
                            _jpg_buf_len = fb->len;
                        }
                        fr_encode = esp_timer_get_time();
                    }
                    dl_matrix3du_free(image_matrix);
                }
            }
        }
        // Envio dos dados
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(fb){
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        } else if(_jpg_buf){
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if(res != ESP_OK){
            break;
        }
        int64_t fr_end = esp_timer_get_time();

        int64_t ready_time = (fr_ready - fr_start)/1000;
        int64_t face_time = (fr_face - fr_ready)/1000;
        int64_t recognize_time = (fr_recognize - fr_face)/1000;
        int64_t encode_time = (fr_encode - fr_recognize)/1000;
        int64_t process_time = (fr_encode - fr_start)/1000;
        
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
        Serial.printf("MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps), %u+%u+%u+%u=%u %s%d\n",
            (uint32_t)(_jpg_buf_len),
            (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
            avg_frame_time, 1000.0 / avg_frame_time,
            (uint32_t)ready_time, (uint32_t)face_time, (uint32_t)recognize_time, (uint32_t)encode_time, (uint32_t)process_time,
            (detected)?"DETECTED ":"", face_id
        );
    }

    last_frame = 0;
    return res;
}

// Handler para a rota /control (ajuste de configurações da câmera via query params)
static esp_err_t cmd_handler(httpd_req_t *req){
    char*  buf;
    size_t buf_len;
    char variable[32] = {0,};
    char value[32] = {0,};

    // Parseia a query string da URL (ex: /control?var=framesize&val=5)
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char*)malloc(buf_len);
        if(!buf){
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
                httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
            } else {
                free(buf);
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        } else {
            free(buf);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        free(buf);
    } else {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int val = atoi(value); // Converte o valor para inteiro
    sensor_t * s = esp_camera_sensor_get(); // Obtém o ponteiro para o sensor da câmera
    int res = 0;

    // Verifica qual variável está sendo alterada e chama a função correspondente do sensor
    if(!strcmp(variable, "framesize")) {
        if(s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val);
    }
    else if(!strcmp(variable, "quality")) res = s->set_quality(s, val);
    else if(!strcmp(variable, "contrast")) res = s->set_contrast(s, val);
    else if(!strcmp(variable, "brightness")) res = s->set_brightness(s, val);
    else if(!strcmp(variable, "saturation")) res = s->set_saturation(s, val);
    else if(!strcmp(variable, "gainceiling")) res = s->set_gainceiling(s, (gainceiling_t)val);
    else if(!strcmp(variable, "colorbar")) res = s->set_colorbar(s, val);
    else if(!strcmp(variable, "awb")) res = s->set_whitebal(s, val);
    else if(!strcmp(variable, "agc")) res = s->set_gain_ctrl(s, val);
    else if(!strcmp(variable, "aec")) res = s->set_exposure_ctrl(s, val);
    else if(!strcmp(variable, "hmirror")) res = s->set_hmirror(s, val);
    else if(!strcmp(variable, "vflip")) res = s->set_vflip(s, val);
    else if(!strcmp(variable, "awb_gain")) res = s->set_awb_gain(s, val);
    else if(!strcmp(variable, "agc_gain")) res = s->set_agc_gain(s, val);
    else if(!strcmp(variable, "aec_value")) res = s->set_aec_value(s, val);
    else if(!strcmp(variable, "aec2")) res = s->set_aec2(s, val);
    else if(!strcmp(variable, "dcw")) res = s->set_dcw(s, val);
    else if(!strcmp(variable, "bpc")) res = s->set_bpc(s, val);
    else if(!strcmp(variable, "wpc")) res = s->set_wpc(s, val);
    else if(!strcmp(variable, "raw_gma")) res = s->set_raw_gma(s, val);
    else if(!strcmp(variable, "lenc")) res = s->set_lenc(s, val);
    else if(!strcmp(variable, "special_effect")) res = s->set_special_effect(s, val);
    else if(!strcmp(variable, "wb_mode")) res = s->set_wb_mode(s, val);
    else if(!strcmp(variable, "ae_level")) res = s->set_ae_level(s, val);
    else if(!strcmp(variable, "face_detect")) {
        detection_enabled = val;
        if(!detection_enabled) {
            recognition_enabled = 0; // Desabilita reconhecimento se detecção for desabilitada
        }
    }
    else if(!strcmp(variable, "face_enroll")) is_enrolling = val;
    else if(!strcmp(variable, "face_recognize")) {
        recognition_enabled = val;
        if(recognition_enabled){
            detection_enabled = val; // Habilita detecção se reconhecimento for habilitado
        }
    }
    else {
        res = -1; // Variável desconhecida
    }

    if(res){
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

// Handler para a rota /status (retorna JSON com o estado atual da câmera)
static esp_err_t status_handler(httpd_req_t *req){
    static char json_response[1024];

    sensor_t * s = esp_camera_sensor_get();
    char * p = json_response;
    *p++ = '{';

    // Constrói o JSON manualmente
    p+=sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p+=sprintf(p, "\"quality\":%u,", s->status.quality);
    p+=sprintf(p, "\"brightness\":%d,", s->status.brightness);
    p+=sprintf(p, "\"contrast\":%d,", s->status.contrast);
    p+=sprintf(p, "\"saturation\":%d,", s->status.saturation);
    p+=sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
    p+=sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
    p+=sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
    p+=sprintf(p, "\"awb\":%u,", s->status.awb);
    p+=sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
    p+=sprintf(p, "\"aec\":%u,", s->status.aec);
    p+=sprintf(p, "\"aec2\":%u,", s->status.aec2);
    p+=sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
    p+=sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
    p+=sprintf(p, "\"agc\":%u,", s->status.agc);
    p+=sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
    p+=sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
    p+=sprintf(p, "\"bpc\":%u,", s->status.bpc);
    p+=sprintf(p, "\"wpc\":%u,", s->status.wpc);
    p+=sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
    p+=sprintf(p, "\"lenc\":%u,", s->status.lenc);
    p+=sprintf(p, "\"vflip\":%u,", s->status.vflip);
    p+=sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
    p+=sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p+=sprintf(p, "\"colorbar\":%u,", s->status.colorbar);
    p+=sprintf(p, "\"face_detect\":%u,", detection_enabled);
    p+=sprintf(p, "\"face_enroll\":%u,", is_enrolling);
    p+=sprintf(p, "\"face_recognize\":%u", recognition_enabled);
    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

// Handler para a rota raiz / (serve a página HTML principal)
static esp_err_t index_handler(httpd_req_t *req){
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip"); // O HTML está comprimido
    sensor_t * s = esp_camera_sensor_get();
    // Seleciona o HTML correto baseado no modelo do sensor (OV3660 ou OV2640)
    if (s->id.PID == OV3660_PID) {
        return httpd_resp_send(req, (const char *)index_ov3660_html_gz, index_ov3660_html_gz_len);
    }
    return httpd_resp_send(req, (const char *)index_ov2640_html_gz, index_ov2640_html_gz_len);
}

// Função para iniciar o servidor da câmera
void startCameraServer(){
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); // Carrega configuração padrão

    // Definição das rotas (URIs)
    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t status_uri = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t cmd_uri = {
        .uri       = "/control",
        .method    = HTTP_GET,
        .handler   = cmd_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

   httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };


    // Inicializa o filtro de FPS
    ra_filter_init(&ra_filter, 20);
    
    // Configurações da rede neural de detecção facial (MTMN)
    mtmn_config.type = FAST; // Modo rápido
    mtmn_config.min_face = 80; // Tamanho mínimo do rosto
    mtmn_config.pyramid = 0.707;
    mtmn_config.pyramid_times = 4;
    mtmn_config.p_threshold.score = 0.6;
    mtmn_config.p_threshold.nms = 0.7;
    mtmn_config.p_threshold.candidate_number = 20;
    mtmn_config.r_threshold.score = 0.7;
    mtmn_config.r_threshold.nms = 0.7;
    mtmn_config.r_threshold.candidate_number = 10;
    mtmn_config.o_threshold.score = 0.7;
    mtmn_config.o_threshold.nms = 0.7;
    mtmn_config.o_threshold.candidate_number = 1;
    
    // Inicializa a lista de IDs de rostos
    face_id_init(&id_list, FACE_ID_SAVE_NUMBER, ENROLL_CONFIRM_TIMES);
    
    // Inicia o servidor web principal (porta 80 por padrão)
    Serial.printf("Starting web server on port: '%d'\n", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
    }

    // Inicia o servidor de stream em uma porta diferente (porta + 1)
    config.server_port += 1;
    config.ctrl_port += 1;
    Serial.printf("Starting stream server on port: '%d'\n", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}
