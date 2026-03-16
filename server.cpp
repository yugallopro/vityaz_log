#include <iostream>
#include <string>
#include <thread>
#include <sstream>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define PASSWORD "vityaz2024"

std::string g_supabase_url = "";
std::string g_supabase_key = "";

std::string timestamp() {
    time_t now = time(0);
    tm* t = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "[%H:%M:%S]", t);
    return std::string(buf);
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i]=='%' && i+2 < s.size()) {
            int c = strtol(s.substr(i+1,2).c_str(), nullptr, 16);
            out += (char)c; i += 2;
        } else if (s[i]=='+') out += ' ';
        else out += s[i];
    }
    return out;
}

std::string getParam(const std::string& src, const std::string& key) {
    std::string search = key + "=";
    size_t pos = src.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = src.find('&', pos);
    return urlDecode(end == std::string::npos
        ? src.substr(pos) : src.substr(pos, end-pos));
}

std::string sanitizeId(const std::string& s) {
    std::string out;
    for (char c : s)
        if (isalnum(c) || c=='_') { out += c; if (out.size()>=32) break; }
    return out.empty() ? "pc_default" : out;
}

// Chiamata HTTPS a Supabase
void supabaseInsert(const std::string& pcId, const std::string& riga) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return;

    struct addrinfo hints{}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_supabase_url.c_str(), "443", &hints, &res) != 0) {
        SSL_CTX_free(ctx); return;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sock);
    SSL_set_tlsext_host_name(ssl, g_supabase_url.c_str());
    if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl); SSL_CTX_free(ctx); close(sock); return;
    }

    // Escape JSON
    std::string escPc, escRiga;
    for (char c : pcId)  { if(c=='"') escPc+="\\\""; else escPc+=c; }
    for (char c : riga)  { if(c=='"') escRiga+="\\\""; else if(c=='\\') escRiga+="\\\\"; else escRiga+=c; }

    std::string body = "{\"pc_id\":\"" + escPc + "\",\"riga\":\"" + escRiga + "\"}";
    std::string req =
        "POST /rest/v1/logs HTTP/1.1\r\n"
        "Host: " + g_supabase_url + "\r\n"
        "apikey: " + g_supabase_key + "\r\n"
        "Authorization: Bearer " + g_supabase_key + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;

    SSL_write(ssl, req.c_str(), req.size());

    // Leggi risposta (non serve il body, solo per completare la transazione)
    char buf[1024]; int n;
    while ((n = SSL_read(ssl, buf, sizeof(buf)-1)) > 0) {}

    SSL_free(ssl); SSL_CTX_free(ctx); close(sock);
}

// Leggi richiesta HTTP completa
std::string readFullRequest(int sock) {
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos) {
        int n = recv(sock, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n] = 0; req += std::string(buf, n);
    }
    size_t headerEnd = req.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return req;
    int cl = 0;
    size_t clPos = req.find("Content-Length: ");
    if (clPos != std::string::npos) cl = atoi(req.substr(clPos+16).c_str());
    std::string body = req.substr(headerEnd+4);
    while ((int)body.size() < cl) {
        int n = recv(sock, buf, std::min(cl-(int)body.size(),(int)sizeof(buf)-1), 0);
        if (n <= 0) break;
        body += std::string(buf, n);
    }
    return req.substr(0, headerEnd+4) + body;
}

void sendHTTP(int sock, const std::string& ctype, const std::string& body) {
    std::ostringstream r;
    r << "HTTP/1.1 200 OK\r\n"
      << "Content-Type: " << ctype << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n\r\n" << body;
    std::string resp = r.str();
    send(sock, resp.c_str(), resp.size(), 0);
}

void handleHTTP(int sock) {
    std::string req = readFullRequest(sock);
    std::string method, fullpath;
    std::istringstream ss(req);
    ss >> method >> fullpath;

    std::string path, query;
    size_t q = fullpath.find('?');
    if (q != std::string::npos) { path=fullpath.substr(0,q); query=fullpath.substr(q+1); }
    else path = fullpath;

    std::string body;
    size_t bp = req.find("\r\n\r\n");
    if (bp != std::string::npos) body = req.substr(bp+4);

    if (path == "/ping") {
        sendHTTP(sock, "application/json", "{\"status\":\"ok\"}");

    } else if (path == "/send" && method == "POST") {
        std::string key  = getParam(query, "key");
        if (key.empty()) key = getParam(body, "key");
        std::string pcId = sanitizeId(getParam(query, "pc"));
        if (pcId.empty()) pcId = sanitizeId(getParam(body, "pc"));
        if (pcId.empty()) pcId = "pc_default";
        std::string riga = getParam(body, "riga");

        if (key != PASSWORD) {
            sendHTTP(sock, "application/json", "{\"status\":\"error\",\"msg\":\"password errata\"}");
        } else if (riga.empty()) {
            sendHTTP(sock, "application/json", "{\"status\":\"error\",\"msg\":\"riga vuota\"}");
        } else {
            std::string entry = timestamp() + " " + riga;
            // Inserisci in Supabase in un thread separato per non bloccare la risposta
            std::string pcCopy = pcId, entryCopy = entry;
            std::thread([pcCopy, entryCopy](){
                supabaseInsert(pcCopy, entryCopy);
            }).detach();
            std::cout << "[" << pcId << "] " << entry << "\n";
            sendHTTP(sock, "application/json", "{\"status\":\"ok\"}");
        }
    } else {
        sendHTTP(sock, "text/plain", "Not found");
    }
    close(sock);
}

int main() {
    const char* url = getenv("SUPABASE_URL");
    const char* key = getenv("SUPABASE_KEY");
    if (!url || !key) {
        std::cerr << "ERRORE: SUPABASE_URL e SUPABASE_KEY richieste\n";
        return 1;
    }
    g_supabase_url = std::string(url);
    g_supabase_key = std::string(key);
    std::cout << "Supabase: " << g_supabase_url << "\n";

    int port = 8080;
    const char* envPort = getenv("PORT");
    if (envPort) port = atoi(envPort);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    bind(fd, (sockaddr*)&addr, sizeof(addr));
    listen(fd, 32);
    std::cout << "=== Vityaz Server porta " << port << " ===\n";

    while (true) {
        int client = accept(fd, NULL, NULL);
        if (client >= 0) std::thread(handleHTTP, client).detach();
    }
    return 0;
}
