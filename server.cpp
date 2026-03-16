#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
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

std::mutex g_mutex;
std::map<std::string, std::vector<std::string>> g_cache;

std::string g_supabase_url  = "";  // es: zyhvckzlwdoxubcgluzr.supabase.co
std::string g_supabase_key  = "";  // anon key

// ── HTTPS request a Supabase ──────────────────────────────────────

std::string httpsRequest(const std::string& host, const std::string& path,
                         const std::string& method, const std::string& body,
                         const std::string& apiKey) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return "";

    struct addrinfo hints{}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(host.c_str(), "443", &hints, &res);

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sock);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl); SSL_CTX_free(ctx); close(sock);
        return "";
    }

    // Costruisci richiesta HTTP
    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "apikey: " + apiKey + "\r\n";
    req += "Authorization: Bearer " + apiKey + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Prefer: return=representation\r\n";
    if (!body.empty())
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    if (!body.empty()) req += body;

    SSL_write(ssl, req.c_str(), req.size());

    // Leggi risposta
    std::string resp;
    char buf[4096];
    int n;
    while ((n = SSL_read(ssl, buf, sizeof(buf)-1)) > 0) {
        buf[n] = 0;
        resp += std::string(buf, n);
    }

    SSL_free(ssl); SSL_CTX_free(ctx); close(sock);

    // Ritorna solo il body (dopo \r\n\r\n)
    size_t pos = resp.find("\r\n\r\n");
    return pos != std::string::npos ? resp.substr(pos+4) : resp;
}

// ── Supabase DB ops ───────────────────────────────────────────────

void dbInit() {
    // Crea tabella tramite SQL via REST (se non esiste)
    // Supabase ha già la tabella se la creiamo dal dashboard
    std::cout << "Supabase configurato: " << g_supabase_url << "\n";
}

void dbInsert(const std::string& pcId, const std::string& riga) {
    // Escape base JSON
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c=='"') o += "\\\"";
            else if (c=='\\') o += "\\\\";
            else o += c;
        }
        return o;
    };
    std::string body = "{\"pc_id\":\"" + esc(pcId) + "\",\"riga\":\"" + esc(riga) + "\"}";
    httpsRequest(g_supabase_url, "/rest/v1/logs", "POST", body, g_supabase_key);
}

void dbClear(const std::string& pcId) {
    httpsRequest(g_supabase_url,
        "/rest/v1/logs?pc_id=eq." + pcId,
        "DELETE", "", g_supabase_key);
}

void dbLoadPC(const std::string& pcId) {
    std::string resp = httpsRequest(g_supabase_url,
        "/rest/v1/logs?pc_id=eq." + pcId + "&order=id.asc&select=riga",
        "GET", "", g_supabase_key);

    // Parse JSON array: [{"riga":"..."},{"riga":"..."}]
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache[pcId].clear();
    size_t pos = 0;
    while ((pos = resp.find("\"riga\":\"", pos)) != std::string::npos) {
        pos += 8;
        std::string val;
        while (pos < resp.size() && resp[pos] != '"') {
            if (resp[pos]=='\\' && pos+1 < resp.size()) {
                pos++;
                if (resp[pos]=='"') val += '"';
                else if (resp[pos]=='\\') val += '\\';
                else val += resp[pos];
            } else val += resp[pos];
            pos++;
        }
        if (!val.empty()) g_cache[pcId].push_back(val);
    }
}

std::vector<std::string> dbGetPCList() {
    std::string resp = httpsRequest(g_supabase_url,
        "/rest/v1/logs?select=pc_id&order=pc_id.asc",
        "GET", "", g_supabase_key);

    // Deduplica pc_id
    std::vector<std::string> list;
    size_t pos = 0;
    std::string prev;
    while ((pos = resp.find("\"pc_id\":\"", pos)) != std::string::npos) {
        pos += 9;
        std::string val;
        while (pos < resp.size() && resp[pos] != '"') val += resp[pos++];
        if (val != prev && !val.empty()) { list.push_back(val); prev = val; }
    }
    return list;
}

// ── Utility ──────────────────────────────────────────────────────

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
    for (char c : s) {
        if (isalnum(c) || c=='_') out += c;
        if (out.size() >= 32) break;
    }
    return out.empty() ? "pc_default" : out;
}

std::string escapeJSON(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c=='"') out += "\\\"";
        else if (c=='\\') out += "\\\\";
        else out += c;
    }
    return out;
}

// ── HTML ─────────────────────────────────────────────────────────

std::string buildHTML() {
    std::string h;
    h += "<!DOCTYPE html><html lang=\"it\"><head><meta charset=\"UTF-8\">";
    h += "<title>Vityaz Monitor</title><style>";
    h += "body{background:#0a0a0a;color:#00ff41;font-family:monospace;padding:20px;margin:0}";
    h += "h2{letter-spacing:4px;border-bottom:1px solid #00ff41;padding-bottom:8px}";
    h += "#controls{display:flex;align-items:center;gap:16px;margin:12px 0;flex-wrap:wrap}";
    h += "select{background:#111;color:#00ff41;border:1px solid #00ff41;padding:6px 12px;font-family:monospace;font-size:14px;cursor:pointer}";
    h += "button{background:#001a00;color:#00ff41;border:1px solid #00ff41;padding:6px 14px;font-family:monospace;cursor:pointer;font-size:13px}";
    h += "button:hover{background:#003300}";
    h += "#stato{font-size:12px;color:#555}";
    h += "#log{white-space:pre-wrap;font-size:14px;line-height:1.8;margin-top:12px}";
    h += "</style></head><body>";
    h += "<h2>VITYAZ MONITOR</h2>";
    h += "<div id=\"controls\">";
    h += "<select id=\"pcSelect\" onchange=\"changePC()\"><option value=\"\">-- seleziona PC --</option></select>";
    h += "<button onclick=\"refreshPCList()\">Aggiorna lista PC</button>";
    h += "<a id=\"dlLink\" href=\"#\" style=\"display:none\"><button>Scarica log</button></a>";
    h += "<button onclick=\"clearLog()\">Svuota log</button>";
    h += "<span id=\"stato\">-</span>";
    h += "</div>";
    h += "<div id=\"log\"></div>";
    h += "<script>";
    h += "var currentPC='',last=0,pollTimer=null;";
    h += "function refreshPCList(){";
    h += "fetch('/pclist').then(function(r){return r.json()}).then(function(d){";
    h += "var sel=document.getElementById('pcSelect');var prev=sel.value;";
    h += "sel.innerHTML='<option value=\"\">-- seleziona PC --</option>';";
    h += "d.pcs.forEach(function(pc){var o=document.createElement('option');";
    h += "o.value=pc;o.textContent=pc;if(pc===prev)o.selected=true;sel.appendChild(o);});});}";
    h += "function changePC(){var sel=document.getElementById('pcSelect');";
    h += "currentPC=sel.value;last=0;document.getElementById('log').textContent='';";
    h += "document.getElementById('stato').textContent=currentPC?'Caricamento...':'-';";
    h += "var dl=document.getElementById('dlLink');";
    h += "if(currentPC){dl.href='/download/'+currentPC+'?psw=vityaz2024';dl.style.display='inline';}";
    h += "else{dl.style.display='none';}";
    h += "if(pollTimer)clearTimeout(pollTimer);if(currentPC)poll();}";
    h += "function poll(){if(!currentPC)return;";
    h += "fetch('/data?pc='+encodeURIComponent(currentPC)+'&last='+last)";
    h += ".then(function(r){return r.json()}).then(function(d){";
    h += "if(d.righe&&d.righe.length>0){var log=document.getElementById('log');";
    h += "d.righe.forEach(function(r){log.textContent+=r+'\\n';});";
    h += "last=d.totale;window.scrollTo(0,document.body.scrollHeight);}";
    h += "document.getElementById('stato').textContent='LIVE ['+currentPC+'] righe: '+last;";
    h += "pollTimer=setTimeout(poll,2000);})";
    h += ".catch(function(){document.getElementById('stato').textContent='Riconnessione...';";
    h += "pollTimer=setTimeout(poll,3000);});}";
    h += "function clearLog(){if(!currentPC){alert('Seleziona prima un PC');return;}";
    h += "if(!confirm('Svuotare il log di '+currentPC+'?'))return;";
    h += "fetch('/clear?psw=vityaz2024&pc='+encodeURIComponent(currentPC),{method:'POST'})";
    h += ".then(function(r){return r.json()})";
    h += ".then(function(d){if(d.status==='ok'){document.getElementById('log').textContent='';last=0;}});}";
    h += "refreshPCList();";
    h += "</script></body></html>";
    return h;
}

// ── JSON builders ─────────────────────────────────────────────────

std::string buildPCListJSON() {
    auto list = dbGetPCList();
    std::string out = "{\"pcs\":[";
    bool first = true;
    for (auto& pc : list) {
        if (!first) out += ",";
        out += "\"" + escapeJSON(pc) + "\"";
        first = false;
    }
    out += "]}";
    return out;
}

std::string buildDataJSON(const std::string& pcId, int lastSeen) {
    bool needLoad = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        needLoad = (g_cache.find(pcId) == g_cache.end());
    }
    if (needLoad) dbLoadPC(pcId);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(pcId);
    if (it == g_cache.end()) return "{\"totale\":0,\"righe\":[]}";
    auto& lines = it->second;
    std::ostringstream oss;
    oss << "{\"totale\":" << lines.size() << ",\"righe\":[";
    bool first = true;
    for (int i = lastSeen; i < (int)lines.size(); i++) {
        if (!first) oss << ",";
        oss << "\"" << escapeJSON(lines[i]) << "\"";
        first = false;
    }
    oss << "]}";
    return oss.str();
}

// ── HTTP server ───────────────────────────────────────────────────

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
    int contentLength = 0;
    size_t clPos = req.find("Content-Length: ");
    if (clPos != std::string::npos)
        contentLength = atoi(req.substr(clPos+16).c_str());
    std::string body = req.substr(headerEnd+4);
    while ((int)body.size() < contentLength) {
        int n = recv(sock, buf, std::min((int)(contentLength-(int)body.size()),(int)sizeof(buf)-1), 0);
        if (n <= 0) break;
        body += std::string(buf, n);
    }
    return req.substr(0, headerEnd+4) + body;
}

void sendHTTP(int sock, const std::string& ctype, const std::string& body,
              const std::string& extra = "") {
    std::ostringstream r;
    r << "HTTP/1.1 200 OK\r\n"
      << "Content-Type: " << ctype << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n"
      << extra << "\r\n" << body;
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

    if (path=="/" || path=="/index.html") {
        sendHTTP(sock, "text/html; charset=UTF-8", buildHTML());
    } else if (path=="/ping") {
        sendHTTP(sock, "application/json", "{\"status\":\"ok\"}");
    } else if (path=="/pclist") {
        sendHTTP(sock, "application/json", buildPCListJSON());
    } else if (path=="/data") {
        std::string pc = sanitizeId(getParam(query,"pc"));
        int last=0;
        std::string lv=getParam(query,"last");
        if (!lv.empty()) last=atoi(lv.c_str());
        if (!pc.empty()) sendHTTP(sock,"application/json",buildDataJSON(pc,last));
        else sendHTTP(sock,"application/json","{\"totale\":0,\"righe\":[]}");
    } else if (path.find("/download/")==0) {
        std::string psw=getParam(query,"psw");
        if (psw!=PASSWORD) {
            sendHTTP(sock,"application/json","{\"status\":\"error\"}");
        } else {
            std::string pcId=sanitizeId(path.substr(10));
            dbLoadPC(pcId);
            std::lock_guard<std::mutex> lock(g_mutex);
            std::string content;
            for (auto& r2 : g_cache[pcId]) content += r2+"\n";
            std::string disp="Content-Disposition: attachment; filename=\""+pcId+".txt\"\r\n";
            sendHTTP(sock,"text/plain; charset=UTF-8",content,disp);
        }
    } else if (path=="/clear" && method=="POST") {
        std::string psw=getParam(query,"psw");
        std::string pcId=sanitizeId(getParam(query,"pc"));
        if (psw!=PASSWORD||pcId.empty()) {
            sendHTTP(sock,"application/json","{\"status\":\"error\"}");
        } else {
            dbClear(pcId);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_cache[pcId].clear();
            sendHTTP(sock,"application/json","{\"status\":\"ok\"}");
        }
    } else if (path=="/send" && method=="POST") {
        std::string key=getParam(query,"key");
        if (key.empty()) key=getParam(body,"key");
        std::string pcId=sanitizeId(getParam(query,"pc"));
        if (pcId.empty()) pcId=sanitizeId(getParam(body,"pc"));
        if (pcId.empty()) pcId="pc_default";
        std::string riga=getParam(body,"riga");
        if (key!=PASSWORD) {
            sendHTTP(sock,"application/json","{\"status\":\"error\",\"msg\":\"password errata\"}");
        } else if (riga.empty()) {
            sendHTTP(sock,"application/json","{\"status\":\"error\",\"msg\":\"riga vuota\"}");
        } else {
            std::string entry=timestamp()+" "+riga;
            { std::lock_guard<std::mutex> lock(g_mutex); g_cache[pcId].push_back(entry); }
            dbInsert(pcId,entry);
            std::cout<<"["<<pcId<<"] "<<entry<<"\n";
            sendHTTP(sock,"application/json","{\"status\":\"ok\"}");
        }
    } else {
        sendHTTP(sock,"text/plain","Not found");
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
    dbInit();

    int port=8080;
    const char* envPort=getenv("PORT");
    if (envPort) port=atoi(envPort);
    int fd=socket(AF_INET,SOCK_STREAM,0);
    int opt=1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons(port);
    bind(fd,(sockaddr*)&addr,sizeof(addr));
    listen(fd,32);
    std::cout<<"=== Vityaz Server porta "<<port<<" ===\n";
    while(true){
        int client=accept(fd,NULL,NULL);
        if(client>=0) std::thread(handleHTTP,client).detach();
    }
    return 0;
}
