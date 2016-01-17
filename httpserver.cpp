/*
  TODO List:
  work with arbitrarily large header sizes
  thread for each socket?
  select and thread the work?
  non-blocking socket stuff of some sort, at any rate
*/


#include <stdio.h>
#include <stdlib.h>
#include <direct.h>
#include <errno.h>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <Wincrypt.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

#if _WIN32
#define strtok_r strtok_s
#endif

#define IPv4MaxPacketSize 65535

#define MAX_CLIENTS 4

typedef unsigned int uint;

const char* HostAddress = "127.0.0.1";
char* DEFAULT_PORT = "9037";


typedef enum {
    PROTOCOL_WebSocket = 1,
    PROTOCOL_NotWebSocket
} socketType;

typedef struct {
    SOCKET Socket;
    socketType Type;
} socketData;

typedef struct {
    socketData SocketData;
} ThreadContext;

SOCKET ListenSocket;
socketData ClientSockets[MAX_CLIENTS]= {0};

void
WriteFile(char* filename, unsigned char* data, long length)
{
    FILE *fp = fopen(filename, "w+b");
    fwrite(data, 1, length, fp);
    fclose(fp);
}

int LastIndexOf(char* str, char lookingFor)
{
    int index = strlen(str);
    while(*(str + (index--)) != lookingFor || index != 0);
    return index;
}

int
strends(char* longStr, char* endSequence)
{
    return strcmp(longStr + (strlen(longStr) - strlen(endSequence)), endSequence) == 0;
}

void
strreplace(char* str, char find, char replace)
{
    while(*str)
    {
        if (*str == find)
        {
            *str = replace;
        }
        ++str;
    }
}

char* PathFilename(char* path, int length)
{
    char* cursor = path + length;
    char backslash = '\\';
    char forwardslash = '/';

    //walk cursor back from the end until a slash is found
    while(cursor > path)
    {
        if (*cursor == backslash || *cursor == forwardslash)
        {
            return cursor+1;
        }
        
        cursor -= 1;
    }

    return cursor;
}

HANDLE ClientListLock;

int
AddSocket(socketData newSocket)
{
    WaitForSingleObject( 
        ClientListLock,
        INFINITE);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!ClientSockets[i].Socket)
        {
            ClientSockets[i] = newSocket;
            return 1;
        }
    }

    ReleaseMutex(ClientListLock);
    
    return 0;
}

int
ClearSocket(socketData oldSocketData)
{
    WaitForSingleObject( 
        ClientListLock,
        INFINITE);
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if (ClientSockets[i].Socket == oldSocketData.Socket)
        {
            ClientSockets[i].Socket = 0;
            return 1;
        }
    }

    ReleaseMutex(ClientListLock);

    return 0;
}

void
ServerStartup()
{
    WSADATA SocketsData;
    if (WSAStartup(MAKEWORD(2,2), &SocketsData) != NO_ERROR)
    {
        printf("Error at winsock startup\n");
        return;
    }

    addrinfo *result = 0;
    addrinfo hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, DEFAULT_PORT, &hints, &result) != 0)
    {
        printf("get addr info failed");
        WSACleanup();
        return;
    }
    
    ListenSocket = socket(result->ai_family,
                          result->ai_socktype,
                          result->ai_protocol);

    if (ListenSocket == INVALID_SOCKET) {
        printf("failed to create socket");
        freeaddrinfo(result);
        WSACleanup();
        return;
    }
    
    if (bind(ListenSocket,
             result->ai_addr,
             (int)result->ai_addrlen)==SOCKET_ERROR)
    {
        printf("Error on server bind.\n");
        return;
    }
    if (listen(ListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        printf("Error listening on socket.\n");
        return;
    }

    printf("Listening on port: %s", DEFAULT_PORT);
}

void
ServerDispose()
{
    closesocket(ListenSocket);
}

SOCKET AcceptClient()
{
    sockaddr_in ClientAddress = {0};
    SOCKET ClientSocket = accept(ListenSocket,
                                 (sockaddr*)&ClientAddress,
                                 NULL);
    printf("Client connected on socket %x. Address %s:%d\n",
           ClientSocket,
           inet_ntoa(ClientAddress.sin_addr),
           ntohs(ClientAddress.sin_port));
    return ClientSocket;
}

void
ServeRequest(SOCKET ClientSocket, char* filename, char* contentType, char* contentDisposition)
{
    strreplace(filename, '/', '\\');
    
    //respond with the matching file
    char* responseFormat =
        "HTTP/1.1 200 OK\n"
        "Content-length: %d\n"
        "Content-type: %s\n"
        "%s\n";
    char responseHeader[BUFSIZ];

    if (strcmp(filename, "") == 0)
    {
        filename = "index.html";
    }
    
    printf("Sending: %s\n", filename);
    FILE* contentFile = fopen(filename, "r");
    if (contentFile == 0)
    {
        //404
        printf("404: file not found");
    }
    else
    {
        WIN32_FILE_ATTRIBUTE_DATA fileData = {0};
        GetFileAttributesEx(
            filename,
            GetFileExInfoStandard,
            &fileData);

        int contentLength = fileData.nFileSizeLow;
        
        char* content = (char*)malloc(sizeof(char)*contentLength);
        fread(content, 1, contentLength, contentFile);

        char contentDispositionHeader[50];
        contentDispositionHeader[0] = '\0';
        if (contentDisposition != 0)
        {
            char* contentDispositionHeaderFormat = "Content-disposition: %s\n";
            sprintf(contentDispositionHeader, contentDispositionHeaderFormat, contentDisposition);
        }
            
        sprintf(responseHeader, responseFormat, contentLength, contentType, contentDispositionHeader);
        
        int headerLength = strlen(responseHeader);

        printf(responseHeader);
        
        send(ClientSocket, responseHeader, headerLength, 0);

        
        printf("File Size: %d\n", contentLength);
        int sentAmount = 0;
        while (sentAmount < contentLength)
        {
            sentAmount += send(ClientSocket,
                               content+sentAmount,
                               contentLength, 0);
        }
        fclose(contentFile);
        free(content);
    }   
}

void
SendHTML(SOCKET ClientSocket, char* filename)
{
    ServeRequest(ClientSocket, filename, "text/html; charset=UTF-8", 0);
}

void
SendFile(SOCKET ClientSocket, char* filepath)
{
    char* filename = PathFilename(filepath, strlen(filepath));
    char* dispositionFormat = "attachment; filename=%s";
    char* disposition = (char*)malloc(sizeof(char)*(strlen(dispositionFormat) +
                                                    strlen(filename) +
                                                    1));
    sprintf(disposition, dispositionFormat, filename);
    ServeRequest(ClientSocket, filepath, "application/octet-stream", disposition);
    free(disposition);
}

void
Respond(SOCKET ClientSocket, char* Request)
{
    printf("Begin Responding...\n");
    char* HttpRequest;
    int index = 0;
    char *wordEnd, *lineEnd;
    char* verb;
    char* path;
    char* version;

    size_t RequestSize = strlen(Request)+1;
    HttpRequest = (char*)malloc(sizeof(char)*RequestSize);
    memcpy(HttpRequest, Request, RequestSize);
    
    char* FirstLine = strtok_r(Request, "\n", &lineEnd);
    
    verb = strtok_r(FirstLine, " ", &wordEnd);
    path = strtok_r(0, " ", &wordEnd);
    version = strtok_r(0, " ", &wordEnd);

    if (strcmp("GET", verb) == 0)
    {
        //TODO: make this check for a file extension..? like if it's just an empty path return index.html, for now forget about that       
        if (strlen(path+1) == 0)
        {
            SendHTML(ClientSocket, "index.html");
        }
        else if (strends(path, ".html"))
        {
            SendHTML(ClientSocket, path+1);
        }
        else
        {
            SendFile(ClientSocket, path+1);
        }
    }
    free(HttpRequest);
}

int
main(int argc, char** argv)
{
    int argIndex = 1;
    //
    while(argIndex < argc)
    {
        if (strcmp("-port", argv[argIndex]) == 0)
        {
            argIndex += 1;
            if (argIndex >= argc)
            {
                printf("-port value not specified.\n");
                return 0;
            }

            int portNumber = atoi(argv[argIndex]);
            int MaxPort = 65535;

            if (portNumber < 0 ||portNumber > MaxPort)
            {
                printf("Port value out of range.\n");
                return 0;
            }
            
            DEFAULT_PORT = argv[argIndex];
        }
        else if (strcmp("-path", argv[argIndex]) == 0)
        {
            argIndex += 1;
            if (argIndex >= argc)
            {
                printf("-path value not specified.\n");
                return 0;
            }

            if (chdir(argv[argIndex]) == 0)
            {
            }
            else
            {
                int chdirErrorValue = errno;
                printf("Problem changing directories: %d", chdirErrorValue);
                return 0;
            }
        }
        else
        {
            printf("Unrecognized flag: %s", argv[argIndex]);
            return 0;
        }

        argIndex += 1;
    }
    
    ClientListLock = CreateMutex(
        NULL,
        FALSE,
        NULL);

    //threads aren't doing anything right now
    HANDLE Threads[MAX_CLIENTS];
    
    ServerStartup();

    while(true)
    {
        SOCKET ClientSocket = AcceptClient();
        
        char requestbuf[BUFSIZ] = {0};
        char responsebuf[BUFSIZ] = {0};
        char encoded[BUFSIZ] = {0};

        //modify this to accept arbitrary input size
        //also have a timeout
        int received = recv(ClientSocket, requestbuf, BUFSIZ, 0);
        printf("%s", requestbuf);
        Respond(ClientSocket, requestbuf);
        closesocket(ClientSocket);
    }
    
    ServerDispose();
    return 0;
}
