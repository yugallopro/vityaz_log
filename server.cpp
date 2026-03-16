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
#include <libpq-fe.h>

#define PASSWORD "vityaz2024"

std::mutex g_mutex;
std::map<std::string, std::vector<std::string>> g_cache;
std::string g_dbUrl = "";

// ── Database ─────────────────────────────────────────────────────

PGconn* dbConnect() {
    return PQconnectdb(g_dbUrl.c_str());
}

void dbInit() {
    PGconn* conn = dbConnect();
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "DB errore: " << PQerrorMessage(conn) << "\n";
        PQfinish(conn);
        return;
    }
    // Crea tabella se non esiste
    PQexec(conn,
        "CREATE TABLE IF NOT EXISTS logs ("
        "  id SERIAL PRIMARY KEY,"
        "  pc_id TEXT NOT NULL,"
        "  riga TEXT NOT NULL,"
        "  ts TIMESTAMP DEFAULT NOW()"
        ");"
    );
    std::cout << "DB connesso e tabella pronta\n";
    PQfinish(conn);
}

void dbInsert(const std::string& pcId, const std::string& riga) {
    PGconn* conn = dbConnect();
    if (PQstatus(conn) != CONNECTION_OK) { PQfinish(conn); return; }
    const char* params[2] = { pcId.c_str(), riga.c_str() };
    PQexecParams(conn,
        "INSERT INTO logs (pc_id, riga) VALUES ($1, $2)",
        2, NULL, params, NULL, NULL, 0);
    PQfinish(conn);
}

void dbClear(const std::string& pcId) {
    PGconn* conn = dbConnect();
    if (PQstatus(conn) != CONNECTION_OK) { PQfinish(conn); return; }
    const char* params[1] = { pcId.c_str() };
    PQexecParams(conn, "DELETE FROM logs WHERE pc_id=$1",
        1, NULL, params, NULL, NULL, 0);
    PQfinish(conn);
}

// Carica tutte le righe di un PC dal DB in cache
void dbLoadPC(const std::string& pcId) {
    PGconn* conn = dbConnect();
    if (PQstatus(conn) != CONNECTION_OK) { PQfinish(conn); return; }
    const char* params[1] = { pcId.c_str() };
    PGresult* res = PQexecParams(conn,
        "SELECT riga FROM logs WHERE pc_id=$1 ORDER BY id ASC",
        1, NULL, params, NULL, NULL, 0);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache[pcId].clear();
    for (int i = 0; i < PQntuples(res); i++)
        g_cache[pcId].push_back(PQgetvalue(res, i, 0));
    PQclear(res);
    PQfinish(conn);
}

std::vector<std::string> dbGetPCList() {
    PGconn* conn = dbConnect();
    std::vector<std::string> list;
    if (PQstatus(conn) != CONNECTION_OK) { PQfinish(conn); return list; }
    PGresult* res = PQexec(conn,
        "SELECT DISTINCT pc_id FROM logs ORDER BY pc_id");
    for (int i = 0; i < PQntuples(res); i++)
        list.push_back(PQgetvalue(res, i, 0));
    PQclear(res);
    PQfinish(conn);
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
    h += "var sel=document.getElementById('pcSelect');";
    h += "var prev=sel.value;";
    h += "sel.innerHTML='<option value=\"\">-- seleziona PC --</option>';";
    h += "d.pcs.forEach(function(pc){";
    h += "var o=document.createElement('option');o.value=pc;o.textContent=pc;";
    h += "if(pc===prev)o.selected=true;sel.appendChild(o);});});}";
    h += "function changePC(){";
    h += "var sel=document.getElementById('pcSelect');";
    h += "currentPC=sel.value;last=0;";
    h += "document.getElementById('log').textContent='';";
    h += "document.getElementById('stato').textContent=currentPC?'Caricamento...':'-';";
    h += "var dl=document.getElementById('dlLink');";
    h += "if(currentPC){dl.href='/download/'+currentPC+'?psw=vityaz2024';dl.style.display='inline';}";
    h += "else{dl.style.display='none';}";
    h += "if(pollTimer)clearTimeout(pollTimer);";
    h += "if(currentPC)poll();}";
    h += "function poll(){";
    h += "if(!currentPC)return;";
    h += "fetch('/data?pc='+encodeURIComponent(currentPC)+'&last='+last)";
    h += ".then(function(r){return r.json()})";
    h += ".then(function(d){";
    h += "if(d.righe&&d.righe.length>0){";
    h += "var log=document.getElementById('log');";
    h += "d.righe.forEach(function(r){log.textContent+=r+'\\n';});";
    h += "last=d.totale;window.scrollTo(0,document.body.scrollHeight);}";
    h += "document.getElementById('stato').textContent='LIVE ['+currentPC+'] righe: '+last;";
    h += "pollTimer=setTimeout(poll,2000);})";
    h += ".catch(function(){";
    h += "document.getElementById('stato').textContent='Riconnessione...';";
    h += "pollTimer=setTimeout(poll,3000);});}";
    h += "function clearLog(){";
    h += "if(!currentPC){alert('Seleziona prima un PC');return;}";
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
    // Se la cache e vuota per questo PC, caricala dal DB
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

// ── HTTP ─────────────────────────────────────────────────────────

std::string readFullRequest(int sock) {
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos) {
        int n = recv(sock, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        req += std::string(buf, n);
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
    if (q != std::string::npos) { path = fullpath.substr(0,q); query = fullpath.substr(q+1); }
    else path = fullpath;

    std::string body;
    size_t bp = req.find("\r\n\r\n");
    if (bp != std::string::npos) body = req.substr(bp+4);

    if (path == "/" || path == "/index.html") {
        sendHTTP(sock, "text/html; charset=UTF-8", buildHTML());

    } else if (path == "/ping") {
        sendHTTP(sock, "application/json", "{\"status\":\"ok\"}");

    } else if (path == "/pclist") {
        sendHTTP(sock, "application/json", buildPCListJSON());

    } else if (path == "/data") {
        std::string pc = sanitizeId(getParam(query, "pc"));
        int last = 0;
        std::string lv = getParam(query, "last");
        if (!lv.empty()) last = atoi(lv.c_str());
        if (!pc.empty())
            sendHTTP(sock, "application/json", buildDataJSON(pc, last));
        else
            sendHTTP(sock, "application/json", "{\"totale\":0,\"righe\":[]}");

    } else if (path.find("/download/") == 0) {
        std::string psw = getParam(query, "psw");
        if (psw != PASSWORD) {
            sendHTTP(sock, "application/json", "{\"status\":\"error\"}");
        } else {
            std::string pcId = sanitizeId(path.substr(10));
            dbLoadPC(pcId);
            std::lock_guard<std::mutex> lock(g_mutex);
            std::string content;
            for (auto& r : g_cache[pcId]) content += r + "\n";
            std::string disp = "Content-Disposition: attachment; filename=\"" + pcId + ".txt\"\r\n";
            sendHTTP(sock, "text/plain; charset=UTF-8", content, disp);
        }

    } else if (path == "/clear" && method == "POST") {
        std::string psw = getParam(query, "psw");
        std::string pcId = sanitizeId(getParam(query, "pc"));
        if (psw != PASSWORD || pcId.empty()) {
            sendHTTP(sock, "application/json", "{\"status\":\"error\"}");
        } else {
            dbClear(pcId);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_cache[pcId].clear();
            sendHTTP(sock, "application/json", "{\"status\":\"ok\"}");
        }

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
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_cache[pcId].push_back(entry);
            }
            dbInsert(pcId, entry);
            std::cout << "[" << pcId << "] " << entry << "\n";
            sendHTTP(sock, "application/json", "{\"status\":\"ok\"}");
        }
    } else {
        sendHTTP(sock, "text/plain", "Not found");
    }
    close(sock);
}

int main() {
    // Leggi URL database dalla variabile d'ambiente
    const char* dbUrl = getenv("DATABASE_URL");
    if (!dbUrl) {
        std::cerr << "ERRORE: variabile DATABASE_URL non impostata\n";
        return 1;
    }
    g_dbUrl = std::string(dbUrl);
    dbInit();

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
