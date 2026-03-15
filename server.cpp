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

#define PASSWORD "vityaz2024"

std::mutex g_mutex;
std::vector<std::string> g_log;

std::string timestamp() {
    time_t now = time(0);
    tm* t = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "[%H:%M:%S]", t);
    return std::string(buf);
}

// Decode URL encoding
std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i+2 < s.size()) {
            int c = strtol(s.substr(i+1,2).c_str(), nullptr, 16);
            out += (char)c; i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// Estrai valore da body POST: key=val&key2=val2
std::string getParam(const std::string& body, const std::string& key) {
    std::string search = key + "=";
    size_t pos = body.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = body.find('&', pos);
    return urlDecode(end == std::string::npos
        ? body.substr(pos)
        : body.substr(pos, end - pos));
}

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
        document.getElementById('stato').textContent = 'Riconnessione...';
        setTimeout(poll, 3000);
    });
}
poll();
</script>
</body>
</html>)";
}

std::string buildJSON(int lastSeen) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ostringstream oss;
    oss << "{\"totale\":" << g_log.size() << ",\"righe\":[";
    bool first = true;
    for (int i = lastSeen; i < (int)g_log.size(); i++) {
        if (!first) oss << ",";
        std::string escaped;
        for (char c : g_log[i]) {
            if (c=='"') escaped+="\\\"";
            else if (c=='\\') escaped+="\\\\";
            else escaped+=c;
        }
        oss << "\"" << escaped << "\"";
        first = false;
    }
    oss << "]}";
    return oss.str();
}

void sendHTTP(int sock, int code, const std::string& ctype, const std::string& body) {
    std::string status = (code==200) ? "200 OK" : "404 Not Found";
    std::ostringstream r;
    r << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: " << ctype << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n\r\n"
      << body;
    std::string resp = r.str();
    send(sock, resp.c_str(), resp.size(), 0);
}

void handleHTTP(int sock) {
    // Leggi tutta la richiesta
    std::string req;
    char buf[8192] = {};
    int n = recv(sock, buf, sizeof(buf)-1, 0);
    if (n > 0) req = std::string(buf, n);

    // Estrai metodo e path
    std::string method, path;
    std::istringstream ss(req);
    ss >> method >> path;

    // Estrai body (dopo \r\n\r\n)
    std::string body;
    size_t bodyPos = req.find("\r\n\r\n");
    if (bodyPos != std::string::npos)
        body = req.substr(bodyPos + 4);

    if (path == "/" || path == "/index.html") {
        sendHTTP(sock, 200, "text/html; charset=UTF-8", buildHTML());

    } else if (path.find("/data") == 0) {
        int last = 0;
        size_t q = path.find("last=");
        if (q != std::string::npos) last = atoi(path.c_str() + q + 5);
        sendHTTP(sock, 200, "application/json", buildJSON(last));

    } else if (path == "/send" && method == "POST") {
        std::string key  = getParam(body, "key");
        std::string riga = getParam(body, "riga");

        if (key != PASSWORD) {
            sendHTTP(sock, 200, "application/json", "{\"status\":\"error\",\"msg\":\"password errata\"}");
        } else if (riga.empty()) {
            sendHTTP(sock, 200, "application/json", "{\"status\":\"error\",\"msg\":\"riga vuota\"}");
        } else {
            std::string entry = timestamp() + " " + riga;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_log.push_back(entry);
                if (g_log.size() > 500) g_log.erase(g_log.begin());
            }
            std::cout << entry << "\n";
            sendHTTP(sock, 200, "application/json", "{\"status\":\"ok\"}");
        }

    } else {
        sendHTTP(sock, 404, "text/plain", "Not found");
    }

    close(sock);
}

int main() {
    int port = 8080;
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
    listen(fd, 32);

    std::cout << "=== Vityaz Server HTTP sulla porta " << port << " ===\n";

    while (true) {
        int client = accept(fd, NULL, NULL);
        if (client >= 0)
            std::thread(handleHTTP, client).detach();
    }
    return 0;
}
