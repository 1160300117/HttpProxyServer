#include <Windows.h>
#include <process.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#pragma comment(lib,"Ws2_32.lib")
#define MAXSIZE 65507 // 发送数据报文的最大长度
#define HTTP_PORT 80 // http 服务器端口

struct HttpHeader {
    char method[4];
    char url[1024];
    char host[1024];
    char cookie[1024 * 10];
    HttpHeader() {
        ZeroMemory(this, sizeof(HttpHeader));
    }
};
struct ProxyParam {
    SOCKET clientSocket;
    SOCKET serverSocket;
};

BOOL InitSocket();
void ParseHttpHead(char *buffer, HttpHeader * httpHeader);
BOOL ConnectToServer(SOCKET *serverSocket, char *host);
unsigned int __stdcall ProxyThread(LPVOID lpParameter);

/***附加功能实现***/
BOOL ParseDate(char *buffer, char *field, char *tempDate);
void makeNewHTTP(char *buffer, char *value);
void makeFilename(char *url, char *filename);
void makeCache(char *buffer, char *filename);
void getCache(char *buffer, char *filename);

// 相关参数
SOCKET ProxyServer;
sockaddr_in ProxyServerAddr;
const int ProxyPort = 10240;
bool haveCache = false;
bool needCache = true;

// 多线程
const int ProxyThreadMaxNum = 20;
HANDLE ProxyThreadHandle[ProxyThreadMaxNum] = {0};
DWORD ProxyThreadDW[ProxyThreadMaxNum] = {0};

int main(int argc, char* argv[]) {
    printf("The proxy server is starting\n");
    printf("Initializing...\n");
    if(!InitSocket()){
        printf("socket initialization failed\n");
        return -1;
    }
    printf("Proxy server is running, listening port %d\n\n", ProxyPort);
    ProxyParam *lpProxyParam;
    HANDLE hThread;
    while(true) {
        lpProxyParam = new ProxyParam();
        if(lpProxyParam == NULL) {
            continue;
        }
        lpProxyParam->clientSocket = accept(ProxyServer, NULL, NULL);
        hThread = (HANDLE)_beginthreadex(NULL, 0, &ProxyThread, (LPVOID)lpProxyParam, 0, 0);
        CloseHandle(hThread);
        Sleep(200);
    }
    closesocket(ProxyServer);
    WSACleanup();
    return 0;
}

BOOL InitSocket() {
    WORD wVersionRequested = MAKEWORD(2, 2);
    WSADATA wsaData;
    if(WSAStartup(wVersionRequested, &wsaData) != 0) {
        printf("Loading winsock failed with error code: %d\n", WSAGetLastError());
        return FALSE;
    }
    if(LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
        printf("Can not find the correct winsock version\n");
        WSACleanup();
        return FALSE;
    }
    ProxyServer= socket(AF_INET, SOCK_STREAM, 0);
    if(INVALID_SOCKET == ProxyServer) {
        printf("Failed to create a socket with error code:%d\n",WSAGetLastError());
        return FALSE;
    }
    ProxyServerAddr.sin_family = AF_INET;
    ProxyServerAddr.sin_port = htons(ProxyPort);
    ProxyServerAddr.sin_addr.S_un.S_addr = INADDR_ANY;
    // 绑定套接字
    if(bind(ProxyServer,(SOCKADDR*)&ProxyServerAddr,sizeof(SOCKADDR)) == SOCKET_ERROR){
        printf("Binding socket failed\n");
        return FALSE;
    }
    // 监听端口
    if(listen(ProxyServer, SOMAXCONN) == SOCKET_ERROR){
        printf("Listening port %d failed",ProxyPort);
        return FALSE;
    }
    return TRUE;
}

void error(LPVOID lpParameter) {
    printf("Close the socket\n");
    Sleep(200);
    closesocket(((ProxyParam*)lpParameter)->clientSocket);
    closesocket(((ProxyParam*)lpParameter)->serverSocket);
    delete lpParameter;
    _endthreadex(0);
}

unsigned int __stdcall ProxyThread(LPVOID lpParameter) {
    char Buffer[MAXSIZE], fileBuffer[MAXSIZE];;
    char *CacheBuffer;
    ZeroMemory(Buffer, MAXSIZE);
    ZeroMemory(fileBuffer, MAXSIZE);
    int recvSize;
    SOCKET client_socket =  ((ProxyParam *)lpParameter)->clientSocket;
    SOCKET server_socket = ((ProxyParam *)lpParameter)->serverSocket;
    // 从客户端接收数据
    recvSize = recv(client_socket, Buffer, MAXSIZE, 0);
    if(recvSize <= 0) {
        error(lpParameter);
    }
    printf("\n>>> Request message\n%s\n", Buffer);
    // 处理头部数据
    HttpHeader* httpHeader = new HttpHeader();
    CacheBuffer = new char[recvSize + 1];
    ZeroMemory(CacheBuffer, sizeof(CacheBuffer));
    memcpy(CacheBuffer, Buffer, recvSize);
    ParseHttpHead(CacheBuffer, httpHeader);
    delete CacheBuffer;
    //网站屏蔽
    if (strcmp(httpHeader->url, "http://jwts.hit.edu.cn/") == 0) {
        printf("You can't visit http://jwts.hit.edu.cn/\n");
        error(lpParameter);
    }
    // 判断是否存在缓存，改造请求报文
    char filename[100];
    ZeroMemory(filename, 100);
    makeFilename(httpHeader->url, filename);
    char *field = "Date";
    char date_str[30];
    ZeroMemory(date_str, 30);
    FILE *in;
    if (fopen_s(&in, filename, "rb") == 0) {
        printf("The proxy server has a corresponding cache under the url %s\n", httpHeader->url);
        fread(fileBuffer, sizeof(char), MAXSIZE, in);
        fclose(in);
        ParseDate(fileBuffer, field, date_str);
        makeNewHTTP(Buffer, date_str);
        printf("\n>>> Modified request message\n%s\n", Buffer);
        haveCache = true;
    }
    // 若状态码是304，则有缓存，获取本地缓存内容
    if (haveCache) {
        getCache(Buffer, filename);
    }
    // 若状态码是200，则无缓存，构造本地缓存
    if (needCache) {
        makeCache(Buffer, filename);
    }
    // 与服务器进行连接
    if(!ConnectToServer(&server_socket, httpHeader->host)) {
        error(lpParameter);
    }
    printf("\nProxy connection host %s succeeded\n", httpHeader->host);
    // 将客户端发送的 HTTP 数据报文转发给目标服务器
    send(server_socket, Buffer, sizeof(Buffer), 0);
    // 等待目标服务器返回数据
    recvSize = recv(server_socket, Buffer, MAXSIZE, 0);
    if(recvSize <= 0) {
        error(lpParameter);
    }
    // 将目标服务器返回的数据直接转发给客户端
    send(client_socket, Buffer, sizeof(Buffer), 0);
    return 0;
}

void ParseHttpHead(char *buffer, HttpHeader * httpHeader) {
    char *p;
    char *ptr;
    const char * delim = "\r\n";
    p = strtok_s(buffer, delim, &ptr);
    if(p[0] == 'G') { // GET 方式
        memcpy(httpHeader->method, "GET", 3);
        memcpy(httpHeader->url, &p[4], strlen(p) - 13);
    } else if(p[0] == 'P') { // POST 方式
        memcpy(httpHeader->method, "POST", 4);
        memcpy(httpHeader->url, &p[5], strlen(p) - 14);
    }
    p = strtok_s(NULL, delim, &ptr);
    while(p) {
        if (p[0] == 'H' && p[1] == 'o' && p[2] == 's' && p[3] == 't') {
            memcpy(httpHeader->host, &p[6], strlen(p) - 6);
        } else if (p[0] == 'C' && p[1] == 'o' && p[2] == 'o' && p[3] == 'k' && p[3] == 'i' && p[3] == 'e') {
            memcpy(httpHeader->cookie, &p[8], strlen(p) -8);
        }
        p = strtok_s(NULL, delim, &ptr);
    }
}

BOOL ConnectToServer(SOCKET *serverSocket, char *host) {
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(HTTP_PORT);
    HOSTENT *hostent = gethostbyname(host);
    if(!hostent) {
        return FALSE;
    }
    in_addr Inaddr = *((in_addr*) *hostent->h_addr_list);
    serverAddr.sin_addr.s_addr = inet_addr(inet_ntoa(Inaddr));
    *serverSocket = socket(AF_INET,SOCK_STREAM, 0);
    if(*serverSocket == INVALID_SOCKET) {
        return FALSE;
    }
    if(connect(*serverSocket,(SOCKADDR *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(*serverSocket);
        return FALSE;
    }
    return TRUE;
}

BOOL ParseDate(char *buffer, char *field, char *tempDate) {
    char *p, *ptr, temp[5];
    const char *delim = "\r\n";
    ZeroMemory(temp, 5);
    p = strtok_s(buffer, delim, &ptr);
    int len = (int) strlen(field) + 2;
    while (p) {
        if (strstr(p, field) != NULL) {
            memcpy(tempDate, &p[len], strlen(p) - len);
            printf("tempDate: %s\n", tempDate);
            return TRUE;
        }
        p = strtok_s(NULL, delim, &ptr);
    }
    return FALSE;
}

void makeNewHTTP(char *buffer, char *value) {
    const char *field = "Host";
    const char *newfield = "If-Modified-Since: ";
    char temp[MAXSIZE];
    ZeroMemory(temp, MAXSIZE);
    char *pos = strstr(buffer, field);
    for (int i = 0; i < strlen(pos); i++) {
        temp[i] = pos[i];
    }
    *pos = '\0';
    while (*newfield != '\0') {
        *pos++ = *newfield++;
    }
    while (*value != '\0') {
        *pos++ = *value++;
    }
    *pos++ = '\r';
    *pos++ = '\n';
    for (int i = 0; i < strlen(temp); i++) {
        *pos++ = temp[i];
    }
}

void makeFilename(char *url, char *filename) {
    char *p = filename;
    while (*url != '\0') {
        if (*url != '/' && *url != ':' && *url != '.') {
            *p++ = *url;
        }
        url++;
    }
}

void makeCache(char *buffer, char *filename) {
    char *p, *ptr, num[10], tempBuffer[MAXSIZE + 1];
    const char * delim = "\r\n";
    ZeroMemory(num, 10);
    ZeroMemory(tempBuffer, MAXSIZE + 1);
    memcpy(tempBuffer, buffer, strlen(buffer));
    p = strtok_s(tempBuffer, delim, &ptr);
    memcpy(num, &p[9], 3);
    if (strcmp(num, "200") == 0) {
        FILE *out;
        if (fopen_s(&out, filename, "wb") == 0) {
            fwrite(buffer, sizeof(char), strlen(buffer), out);
            fclose(out);
        }
        printf("This message has been cached\n");
    }
}

void getCache(char *buffer, char *filename) {
    char *p, *ptr, num[10], tempBuffer[MAXSIZE + 1];
    const char * delim = "\r\n";
    ZeroMemory(num, 10);
    ZeroMemory(tempBuffer, MAXSIZE + 1);
    memcpy(tempBuffer, buffer, strlen(buffer));
    p = strtok_s(tempBuffer, delim, &ptr);
    memcpy(num, &p[9], 3);
    if (strcmp(num, "304") == 0) {
        printf("Get the local cache\n");
        ZeroMemory(buffer, strlen(buffer));
        FILE *in;
        if (fopen_s(&in, filename, "rb") == 0) {
            fread(buffer, sizeof(char), MAXSIZE, in);
            fclose(in);
        }
        needCache = false;
    }
}