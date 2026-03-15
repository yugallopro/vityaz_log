#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <sstream>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

#define PASSWORD   "vityaz2024"
#define TCP_PORT   9000   // client C++ si connette qui
#define HTTP_PORT  8080   // browser si connette qui (Render usa PORT env)

std::mutex g_mutex;
std::vector<std::string> g_log; // tutte le righe ricevute

std::string timestamp() {
    time_t now = time(0);
    tm* t = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "[%H:%M:%S]", t);
    return std::string(buf);
}

void sendStr(int s, const std::string& msg) {
    std::string out = msg + "\n";
    send(s, out.c_str(), out.size(), 0);
}

std::string readLine(int s) {
    std::string result;
    char c;
    while (recv(s, &c, 1, 0) == 1) {
        if (c == '\n') break;
        if (c != '\r') result += c;
    }
    return result;
}

// ── Thread che gestisce il client C++ ───────────────────────────
void handleClient(int sock) {
    // Autenticazione
    std::string pass = readLine(sock);
    if (pass != PASSWORD) {
        sendStr(sock, "NEGATO");
        close(sock);
        return;
    }
    sendStr(sock, "OK");
    std::cout << timestamp() << " Client connesso\n";

    while (true) {
        std::string riga = readLine(sock);
        if (riga.empty()) break;

        std::string entry = timestamp() + " " + riga;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_log.push_back(entry);
            // Tieni al massimo 500 righe
            if (g_log.size() > 500) g_log.erase(g_log.begin());
        }
        std::cout << entry << "\n";
        sendStr(sock, "OK");
    }

    std::cout << timestamp() << " Client disconnesso\n";
    close(sock);
}

// ── Genera la pagina HTML con SSE ────────────────────────────────
std::string buildHTML() {
    return R"(<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="UTF-8">
<title>Vityaz Monitor</title>
<style>
  body { background:#0a0a0a; color:#00ff41; font-family:monospace; padding:20px; margin:0; }
  h2   { letter-spacing:4px; border-bottom:1px solid #00ff41; padding-bottom:8px; }
  #stato { font-size:12px; color:#555; margin:8px 0; }
  #log { white-space:pre-wrap; font-size:14px; line-height:1.8; }
</style>
</head>
<body>
<h2>VITYAZ MONITOR</h2>
<div id="stato">Connessione...</div>
<div id="log"></div>
<script>
var last = 0;
function poll() {
    fetch('/data?last=' + last)
    .then(r => r.json())
    .then(d => {
        if (d.righe && d.righe.length > 0) {
            var log = document.getElementById('log');
            d.righe.forEach(function(r) { log.textContent += r + '\n'; });
            last = d.totale;
            window.scrollTo(0, document.body.scrollHeight);
        }
        document.getElementById('stato').textContent = 'LIVE - righe: ' + last;
        setTimeout(poll, 2000);
    })
    .catch(function() {
        document.getElementById('stato').textContent = 'Errore connessione, riprovo...';
        setTimeout(poll, 3000);
    });
}
poll();
</script>
</body>
</html>)";
}

// ── Genera JSON con le righe nuove ───────────────────────────────
std::string buildJSON(int lastSeen) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ostringstream oss;
    oss << "{\"totale\":" << g_log.size() << ",\"righe\":[";
    bool first = true;
    for (int i = lastSeen; i < (int)g_log.size(); i++) {
        if (!first) oss << ",";
        // Escape base per JSON
        std::string s = g_log[i];
        std::string escaped;
        for (char c : s) {
            if (c == '"') escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else escaped += c;
        }
        oss << "\"" << escaped << "\"";
        first = false;
    }
    oss << "]}";
    return oss.str();
}

// ── Thread HTTP per il browser ───────────────────────────────────
void handleHTTP(int sock) {
    char buf[4096] = {};
    recv(sock, buf, sizeof(buf)-1, 0);
    std::string req(buf);

    // Leggi il path dalla prima riga HTTP
    std::string path = "/";
    size_t get = req.find("GET ");
    if (get != std::string::npos) {
        size_t start = get + 4;
        size_t end   = req.find(' ', start);
        path = req.substr(start, end - start);
    }

    std::string body, ctype;
    if (path == "/" || path == "/index.html") {
        body  = buildHTML();
        ctype = "text/html; charset=UTF-8";
    } else if (path.find("/data") == 0) {
        int last = 0;
        size_t q = path.find("last=");
        if (q != std::string::npos) last = atoi(path.c_str() + q + 5);
        body  = buildJSON(last);
        ctype = "application/json";
    } else {
        body  = "Not found";
        ctype = "text/plain";
    }

    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: " << ctype << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "Connection: close\r\n\r\n"
         << body;
    std::string r = resp.str();
    send(sock, r.c_str(), r.size(), 0);
    close(sock);
}

void tcpServer() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(TCP_PORT);
    bind(fd, (sockaddr*)&addr, sizeof(addr));
    listen(fd, 4);
    std::cout << "TCP in ascolto sulla porta " << TCP_PORT << "\n";
    while (true) {
        int client = accept(fd, NULL, NULL);
        if (client >= 0)
            std::thread(handleClient, client).detach();
    }
}

void httpServer() {
    // Render passa la porta via env PORT
    int port = HTTP_PORT;
    const char* envPort = getenv("PORT");
    if (envPort) port = atoi(envPort);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    bind(fd, (sockaddr*)&addr, sizeof(addr));
    listen(fd, 16);
    std::cout << "HTTP in ascolto sulla porta " << port << "\n";
    while (true) {
        int client = accept(fd, NULL, NULL);
        if (client >= 0)
            std::thread(handleHTTP, client).detach();
    }
}

int main() {
    std::cout << "=== Vityaz Server ===\n";
    std::thread(tcpServer).detach();
    httpServer(); // blocca qui
    return 0;
}
