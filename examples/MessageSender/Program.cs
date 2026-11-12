using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading.Channels;

const int tcpPort = 23;
const int maxMessageBytes = 255;
const int acknowledgementTimeoutSeconds = 5;
const string defaultAddress = "192.168.4.1";

Console.WriteLine("Conecte o computador à rede Wi-Fi T-Display-S3-Message antes de continuar.");
Console.Write($"Conectando em {defaultAddress}... ");

using var tcpClient = new TcpClient();
try
{
    await tcpClient.ConnectAsync(defaultAddress, tcpPort);
}
catch (SocketException ex)
{
    Console.Error.WriteLine($"Não foi possível conectar a {defaultAddress}:{tcpPort}: {ex.Message}");
    Console.Error.WriteLine("Verifique se o computador está conectado ao Wi-Fi da placa e se o IP está correto.");
    return 1;
}

using NetworkStream stream = tcpClient.GetStream();
using var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };
using var reader = new StreamReader(
    stream,
    new UTF8Encoding(false),
    detectEncodingFromByteOrderMarks: false,
    bufferSize: 1024,
    leaveOpen: true);
var responses = Channel.CreateUnbounded<string>();
using var writeLock = new SemaphoreSlim(1, 1);
Task receiveTask = ReceiveServerMessagesAsync();

try
{
    Console.WriteLine("Conectado! Digite uma mensagem e pressione Enter para enviá-la.");

    while (true)
    {
        Console.Write("Mensagem> ");
        string? message = Console.ReadLine();
        if (message is null)
        {
            break;
        }

        if (Encoding.UTF8.GetByteCount(message) > maxMessageBytes)
        {
            Console.WriteLine($"A mensagem é muito longa. Limite: {maxMessageBytes} bytes em UTF-8.");
            continue;
        }

        await WriteToServerAsync(message);

        string? response;
        using (var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(acknowledgementTimeoutSeconds)))
        {
            try
            {
                response = await responses.Reader.ReadAsync(timeout.Token);
            }
            catch (OperationCanceledException)
            {
                Console.Error.WriteLine("Tempo esgotado: a placa não confirmou o recebimento da mensagem.");
                return 1;
            }
            catch (ChannelClosedException)
            {
                Console.Error.WriteLine("Conexão perdida antes da confirmação de recebimento.");
                return 1;
            }
        }

        if (response == "OK")
        {
            Console.WriteLine("Mensagem recebida!");
        }
        else if (response is null)
        {
            Console.Error.WriteLine("Conexão perdida antes da confirmação de recebimento.");
            return 1;
        }
        else
        {
            Console.Error.WriteLine($"A placa não aceitou a mensagem: {response}");
        }
    }
}
catch (IOException ex)
{
    Console.Error.WriteLine($"Conexão perdida: {ex.Message}");
    return 1;
}

tcpClient.Close();
await receiveTask;
return 0;

async Task WriteToServerAsync(string line)
{
    await writeLock.WaitAsync();
    try
    {
        await writer.WriteLineAsync(line);
    }
    finally
    {
        writeLock.Release();
    }
}

async Task ReceiveServerMessagesAsync()
{
    try
    {
        while (true)
        {
            string? line = await reader.ReadLineAsync();
            if (line is null)
            {
                responses.Writer.TryComplete(new IOException("A placa encerrou a conexão."));
                return;
            }

            if (line == "\x01PING")
            {
                await WriteToServerAsync("\x01PONG");
            }
            else
            {
                await responses.Writer.WriteAsync(line);
            }
        }
    }
    catch (IOException ex)
    {
        responses.Writer.TryComplete(ex);
    }
}
