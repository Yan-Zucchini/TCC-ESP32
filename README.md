# Manual do Sistema de Controle de Acesso por Reconhecimento Facial (TCC)

Este documento serve como manual técnico e de usuário para o projeto de Trabalho de Conclusão de Curso (TCC) focado em um sistema de controle de acesso utilizando ESP32-CAM e reconhecimento facial.

## 1. Visão Geral do Projeto

O projeto consiste em um sistema de segurança que utiliza uma câmera ESP32-CAM para capturar imagens, processar e extrair "assinaturas" faciais (embeddings). O sistema opera em conjunto com um servidor backend (Python/Flask) que recebe essas assinaturas para armazenamento e comparação, decidindo se o acesso deve ser liberado ou negado.

### Funcionalidades Principais
- **Detecção Facial**: Identificação da presença de rostos na imagem (Executada na ESP32).
- **Reconhecimento Facial**: Extração de características na ESP32 e comparação matemática no servidor.
- **Controle de Acesso**: Acionamento de LEDs (Verde/Vermelho) indicando permissão ou negação de acesso.
- **Streaming de Vídeo**: Visualização em tempo real da câmera via navegador web.
- **Cadastro de Usuários**: Interface Web de Administração para registrar, renomear e excluir rostos no sistema.

## 2. Requisitos do Sistema

### Hardware Necessário
- **Módulo ESP32-CAM** (Modelo AI-Thinker com 2MB PSRAM).
- **Módulo Conversor USB-Serial (FTDI)** ou **ESP32-CAM-MB**: Para programar o ESP32-CAM.
- **Fonte de Alimentação 5V/9V**: Fonte externa e módulo MB102 para alimentação estável.
- **Jumpers (Fios de conexão)**.
- **LEDs e Resistores**:
  - 1x LED Verde (Acesso Liberado - GPIO 2).
  - 1x LED Vermelho (Acesso Negado - GPIO 14).
  - 2x Resistores de 220Ω ou 330Ω.
- **Protoboard**.

### Software Necessário
- **Arduino IDE (Versão recomendada: 1.8.x ou 2.x)**: Com suporte ao pacote ESP32 versão **1.0.4**.
- **Python 3.x**: Para rodar o servidor backend.
- **Bibliotecas Python**: `flask`, `numpy`.

## 3. Instalação e Configuração

### Passo 1: Configuração da Arduino IDE
1. Instale a Arduino IDE.
2. Em **Arquivo > Preferências**, adicione a URL: `https://dl.espressif.com/dl/package_esp32_index.json`.
3. Em **Gerenciador de Placas**, instale a versão **1.0.4** do pacote "esp32 by Espressif Systems" (essencial para compatibilidade com as bibliotecas de face incluídas).

### Passo 2: Preparação do Código ESP32
1. Abra o arquivo `testeversaoantiga.ino`.
2. Certifique-se de que todos os arquivos `.cpp` e `.h` estão na mesma pasta.
3. Edite as credenciais WiFi:
   ```cpp
   const char* ssid = "SEU_WIFI";
   const char* password = "SUA_SENHA";
   ```

4.  Atualize o IP do servidor no arquivo `app_httpd.cpp`:
      - Encontre as linhas `http.begin("http://[IP]:5000/...");` e altere para o IP atual do seu computador.

### Passo 3: Upload para o ESP32-CAM

1.  Selecione a placa **AI Thinker ESP32-CAM**.
2.  Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**.
3.  Faça o upload.
4.  Conecte o hardware final (LEDs e fonte externa).

### Passo 4: Configuração do Servidor Backend (Python)

1.  Crie e ative um ambiente virtual (`venv`).
2.  Instale as dependências:
    ```bash
    pip install flask numpy
    ```
3.  Execute o servidor:
    ```bash
    python servidor.py
    ```

## 4. Manual de Uso

### Inicialização

1.  Ligue a ESP32. O flash piscará por 2 segundos indicando boot.
2.  Inicie o servidor Python.

### Interface Web (Admin)

1.  Acesse `http://[IP_DO_PC]:5000/admin`.
2.  Você verá o painel de controle e o vídeo da câmera lado a lado.

### Cadastro de Rosto

1.  No painel Admin, digite o nome da pessoa e clique em **"Preparar Registo"**.
2.  Na visualização da câmera, clique no botão **"Enroll Face"**.
3.  Aproxime o rosto. O sistema irá capturar e salvar a assinatura.

### Reconhecimento

1.  Na visualização da câmera, ative **"Face Detection"** e **"Face Recognition"**.
2.  O sistema analisará os rostos automaticamente.
      - **Sucesso:** Quadrado Verde, Nome na tela, LED Verde acende.
      - **Falha:** Quadrado Vermelho, "Desconhecido", LED Vermelho acende.

---

**Autor**: Yan Zucchini
**Instituição**: IFSP
**Ano**: 2025
