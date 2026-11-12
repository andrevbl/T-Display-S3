# MessageReceiver

Este exemplo cria um ponto de acesso Wi-Fi no T-Display-S3 e recebe mensagens
finalizadas por uma quebra de linha pela porta TCP 23. A mensagem mais recente
é exibida na tela.

## Compilar e gravar

No arquivo `platformio.ini`, selecione este exemplo alterando
`default_envs = factory` para `default_envs = MessageReceiver`. Em seguida,
execute estes comandos na pasta raiz do repositório:

```powershell
pio run
pio run -t upload
```

Primeiro, conecte o computador Windows à rede Wi-Fi `T-Display-S3-Message`.
A senha padrão é `message123`. Depois, execute o programa de envio pelo console:

```powershell
dotnet run --project examples\MessageSender\MessageSender.csproj
```

Quando solicitado, informe o endereço IP da placa (pressione Enter para usar
`192.168.4.1`). Em seguida, digite as mensagens e pressione Enter para enviar
cada uma. O programa só informa que a mensagem foi enviada após receber a
confirmação da placa; se não receber resposta em cinco segundos, informa que
não foi possível confirmar o recebimento. A placa verifica a conexão a cada
cinco segundos e encerra a conexão se não receber resposta ao heartbeat em
outros cinco segundos. Mensagens com mais de 255 bytes em UTF-8 são rejeitadas.
O percentual da bateria é uma estimativa baseada na tensão e é atualizado a
cada 30 segundos; pode variar com a carga e não representa uma medição exata da
capacidade restante. A leitura da bateria não é confiável com o USB conectado.
Para alterar as credenciais do Wi-Fi, edite `AP_SSID`
e `AP_PASSWORD` em `MessageReceiver.ino` (a senha deve ter pelo menos oito
caracteres).
