// ============================================================================
//  W3dLlmControl.cpp -- el MCP de Whisk3D: un servidor del Model Context Protocol por la entrada y la salida estandar.
//
//  "whisk3d --mcp" abre el editor como siempre, con su ventana, y ademas atiende pedidos JSON-RPC 2.0 (uno por linea)
//  por stdin; las respuestas salen por stdout. Lo lanza el cliente MCP (Claude Code, Claude Desktop, otro): la IA usa
//  el mismo editor que se ve en la pantalla, y lo que hace queda en el Ctrl+Z como si lo hubiera hecho una persona.
//  Las herramientas son el editor mismo: las acciones de los menus (agregar, importar, abrir, guardar), unas propias
//  (ver la escena, sacar una captura, seleccionar, transformar) y, para todo lo demas, los comandos del harness de
//  pruebas (W3dRunCommand, en W3dScript.cpp).
//
//  Solo en las PC: W3D_MCP lo define el CMakeLists de escritorio. Symbian, Dreamcast, 3DS, Android y la web no
//  compilan nada de esto. En la PC, sin --mcp, cuesta una comparacion por cuadro.
//
//  Los pedidos los lee un hilo (stdin bloquea) y los ejecuta el hilo principal entre cuadros: el contexto de OpenGL y
//  el editor son suyos. Lo que los comandos imprimen con printf iria a stdout y romperia el protocolo: stdout se
//  redirige a un archivo temporal, de donde se lee lo que imprimio cada comando, y el protocolo sale por una copia del
//  stdout original.
// ============================================================================
#ifdef W3D_MCP

#include "extensions/W3dLlmControl.h"
#include "extensions/W3dLlmWorkspace.h"
#include "test/W3dScript.h"
#include "io/JsonW3d.h"            // JVal (el arbol del JSON)
#include "io/lectura-escritura.h"  // ImportModeloPorExtension
#include "objects/Objects.h"
#include "objects/ObjectMode.h"
#include "ViewPorts/ViewPorts.h"
#include "ViewPorts/ViewPort3D.h"
#include "ViewPorts/LayoutInput.h"
#include "WhiskUI/widgets/PopupMenu.h"   // MenuPantallaW
#include "undo/Undo.h"
#include "w3dGraphics.h"
#include "w3dVersion.h"
#include "app/W3dAtajos.h"
#include "app/controles.h"
#include "app/variables.h"
#include "io/CambiosProyecto.h"
#include "base/W3dInteractionState.h"
#include "W3dLlmCommandCatalog.inc"
#include <SDL2/SDL.h>
#include <string>
#include <deque>
#include <vector>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <map>
#ifdef _WIN32
    #include <io.h>
    #include <fcntl.h>
    #define W3D_DUP    _dup
    #define W3D_DUP2   _dup2
    #define W3D_FILENO _fileno
    #define W3D_FDOPEN _fdopen
    #define W3D_LSEEK  _lseek
    #define W3D_READ   _read
    #define W3D_TRUNC  _chsize
#else
    #include <unistd.h>
    #define W3D_DUP    dup
    #define W3D_DUP2   dup2
    #define W3D_FILENO fileno
    #define W3D_FDOPEN fdopen
    #define W3D_LSEEK  lseek
    #define W3D_READ   read
    #define W3D_TRUNC  ftruncate
#endif

extern bool g_redraw;
extern int W3dPantallaAlto;
void AddCube(); void AddPlane(); void AddCircle(); void AddUVSphere(); void AddCone(); void AddCylinder();
void AddVertex(); void AddEmpty(); void AddCamera(); void AddLight();

// ============================================================================
//  La entrada y la salida
// ============================================================================
static bool  gActivo = false;
static FILE* gProto = NULL;       // el stdout original: por aca sale el protocolo
static int   gFdCaptura = -1;     // lo que queda de stdout: un archivo temporal con lo que imprimen los comandos
static SDL_mutex* gCola = NULL;
static std::deque<std::string> gPedidos;
static SDL_atomic_t gCerrado;     // stdin se cerro: el cliente se fue
static SDL_atomic_t gTipoEvento;  // el evento que despierta al loop (que en reposo duerme esperando eventos)
static unsigned gToolCallsDesdeContexto = 0;

static void Despertar() {
    const int t = SDL_AtomicGet(&gTipoEvento);
    if (t <= 0) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = (Uint32)t;
    SDL_PushEvent(&e);
}
static void Encolar(std::string& linea) {
    while (!linea.empty() && (linea[linea.size() - 1] == '\n' || linea[linea.size() - 1] == '\r')) linea.erase(linea.size() - 1);
    if (linea.empty()) return;
    SDL_LockMutex(gCola);
    gPedidos.push_back(linea);
    SDL_UnlockMutex(gCola);
    linea.clear();
    Despertar();
}
// el hilo lector: una linea por pedido (una linea larga llega en partes)
static int LeerPedidos(void*) {
    std::string linea;
    char buf[4096];
    while (fgets(buf, sizeof(buf), stdin)) {
        linea += buf;
        if (!linea.empty() && linea[linea.size() - 1] == '\n') Encolar(linea);
    }
    Encolar(linea);
    SDL_AtomicSet(&gCerrado, 1);
    Despertar();
    return 0;
}

bool W3dLlmActivo() { return gActivo; }

bool W3dLlmIniciar(bool terminalMode) {
    fflush(stdout);
    const int fdOut = W3D_FILENO(stdout);
    const int fdProto = W3D_DUP(fdOut);
    if (fdProto < 0) return false;
#ifdef _WIN32
    _setmode(fdProto, _O_BINARY);   // (sin "\r\n": el protocolo es una linea por mensaje)
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    gProto = W3D_FDOPEN(fdProto, "wb");
    FILE* captura = tmpfile();
    if (!gProto || !captura) return false;
    gFdCaptura = W3D_FILENO(captura);
    W3D_DUP2(gFdCaptura, fdOut);
    gCola = SDL_CreateMutex();
    SDL_Thread* hilo = SDL_CreateThread(LeerPedidos, "w3d_mcp", NULL);
    if (!gCola || !hilo) return false;
    SDL_DetachThread(hilo);
    gActivo = true;
    if (terminalMode) {
        fprintf(gProto,
                "{\"type\":\"whisk3d.llm.ready\",\"command_count\":%u,"
                "\"next\":\"Call guide, then catalog_search; use command/shortcut/input to act.\"}\n",
                kW3dGeneratedCommandCount);
        fflush(gProto);
    }
    return true;
}

// lo que imprimieron los comandos desde la ultima vez (y el archivo vuelve a cero)
static std::string LeerCaptura() {
    fflush(stdout);
    const long fin = (long)W3D_LSEEK(gFdCaptura, 0, SEEK_END);
    std::string s;
    if (fin > 0) {
        s.resize((size_t)fin);
        W3D_LSEEK(gFdCaptura, 0, SEEK_SET);
        const long n = (long)W3D_READ(gFdCaptura, &s[0], (unsigned)fin);
        s.resize(n > 0 ? (size_t)n : 0);
    }
    W3D_TRUNC(gFdCaptura, 0);
    W3D_LSEEK(gFdCaptura, 0, SEEK_SET);
    return s;
}

// ============================================================================
//  JSON: el lector (completo: todos los escapes, \uXXXX a UTF-8) y lo poco que hace falta para escribir
// ============================================================================
struct LectorJson {
    const char* p;
    const char* fin;
    bool error;
    LectorJson(const std::string& s) : p(s.data()), fin(s.data() + s.size()), error(false) {}
    void Blancos() { while (p < fin && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool Comer(char c) { Blancos(); if (p < fin && *p == c) { p++; return true; } error = true; return false; }
    static void Utf8(std::string& s, unsigned c) {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    unsigned Hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            if (p >= fin) { error = true; return 0; }
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else error = true;
        }
        return v;
    }
    std::string Cadena() {
        std::string s;
        if (!Comer('"')) return s;
        while (p < fin && *p != '"') {
            if (*p != '\\') { s += *p++; continue; }
            if (++p >= fin) break;
            const char c = *p++;
            switch (c) {
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case 'r': s += '\r'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'u': {
                    unsigned u = Hex4();
                    if (u >= 0xD800 && u < 0xDC00 && fin - p >= 6 && p[0] == '\\' && p[1] == 'u') {   // (par sustituto)
                        p += 2;
                        const unsigned lo = Hex4();
                        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    Utf8(s, u);
                    break;
                }
                default: s += c; break;   // \" \\ \/
            }
        }
        if (p < fin) p++; else error = true;
        return s;
    }
    JVal* Valor() {
        Blancos();
        JVal* v = new JVal();
        if (p >= fin) { error = true; return v; }
        if (*p == '{') {
            v->tipo = 4; p++;
            Blancos();
            if (p < fin && *p == '}') { p++; return v; }
            while (!error) {
                Blancos();
                const std::string k = Cadena();
                if (!Comer(':')) break;
                JVal*& slot = v->obj[k];
                delete slot;
                slot = Valor();
                Blancos();
                if (p < fin && *p == ',') { p++; continue; }
                Comer('}');
                break;
            }
        } else if (*p == '[') {
            v->tipo = 5; p++;
            Blancos();
            if (p < fin && *p == ']') { p++; return v; }
            while (!error) {
                v->lista.push_back(Valor());
                Blancos();
                if (p < fin && *p == ',') { p++; continue; }
                Comer(']');
                break;
            }
        } else if (*p == '"') {
            v->tipo = 2; v->str = Cadena();
        } else if (fin - p >= 4 && !strncmp(p, "true", 4)) { v->tipo = 3; v->b = true; p += 4; }
        else if (fin - p >= 5 && !strncmp(p, "false", 5)) { v->tipo = 3; v->b = false; p += 5; }
        else if (fin - p >= 4 && !strncmp(p, "null", 4)) { p += 4; }
        else {
            char nbuf[64];
            size_t n = (size_t)(fin - p); if (n > sizeof(nbuf) - 1) n = sizeof(nbuf) - 1;
            memcpy(nbuf, p, n); nbuf[n] = 0;
            char* despues = NULL;
            v->num = strtod(nbuf, &despues);
            if (despues == nbuf) error = true;
            else { v->tipo = 1; p += despues - nbuf; }
        }
        return v;
    }
};
static const JVal* Campo(const JVal* o, const char* k) {
    if (!o || o->tipo != 4) return NULL;
    std::map<std::string, JVal*>::const_iterator it = o->obj.find(k);
    return it == o->obj.end() ? NULL : it->second;
}
static std::string Texto(const JVal* v) { return (v && v->tipo == 2) ? v->str : std::string(); }
static bool Tres(const JVal* v, float out[3]) {
    if (!v || v->tipo != 5 || v->lista.size() != 3) return false;
    for (int i = 0; i < 3; i++) { if (v->lista[i]->tipo != 1) return false; out[i] = (float)v->lista[i]->num; }
    return true;
}
static std::string Esc(const std::string& s) {
    std::string o = "\"";
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;   // (UTF-8 tal cual: es JSON valido)
    }
    return o + "\"";
}
// el id del pedido, tal cual vino (numero o texto), para la respuesta
static std::string IdTexto(const JVal* id) {
    if (!id) return "null";
    if (id->tipo == 2) return Esc(id->str);
    if (id->tipo == 1) {
        char b[48];
        if (id->num == (double)(long long)id->num) snprintf(b, sizeof(b), "%lld", (long long)id->num);
        else snprintf(b, sizeof(b), "%.17g", id->num);
        return b;
    }
    return "null";
}
static std::string Base64(const std::string& d) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        const unsigned v = (unsigned)(unsigned char)d[i] << 16 | (i + 1 < d.size() ? (unsigned)(unsigned char)d[i + 1] << 8 : 0u) |
                           (i + 2 < d.size() ? (unsigned)(unsigned char)d[i + 2] : 0u);
        o += k[(v >> 18) & 63]; o += k[(v >> 12) & 63];
        o += i + 1 < d.size() ? k[(v >> 6) & 63] : '=';
        o += i + 2 < d.size() ? k[v & 63] : '=';
    }
    return o;
}
// ============================================================================
//  La captura en PNG COMPRIMIDO: el PNG del editor (EncodePNG) se guarda sin comprimir, y una captura de la ventana
//  pesaba 6 MB (mas un tercio en base64): demasiado para mandarle a una IA. Aca va achicada y comprimida con deflate
//  (los codigos fijos de Huffman, con busqueda de repeticiones): la interfaz tiene zonas lisas y queda en decenas de KB
// ============================================================================
struct Bits {
    std::string out;
    unsigned acc;
    int n;
    Bits() : acc(0), n(0) {}
    void Poner(unsigned v, int cant) {   // (el primer bit, el de abajo)
        acc |= v << n; n += cant;
        while (n >= 8) { out += (char)(acc & 0xFF); acc >>= 8; n -= 8; }
    }
    void Huffman(unsigned codigo, int cant) {   // (los codigos de Huffman van del bit de arriba)
        unsigned r = 0;
        for (int i = 0; i < cant; i++) r |= ((codigo >> i) & 1u) << (cant - 1 - i);
        Poner(r, cant);
    }
    void Cerrar() { if (n > 0) { out += (char)(acc & 0xFF); acc = 0; n = 0; } }
};
static void Literal(Bits& b, unsigned v) {
    if (v < 144) b.Huffman(0x30 + v, 8);
    else if (v < 256) b.Huffman(0x190 + (v - 144), 9);
    else if (v < 280) b.Huffman(v - 256, 7);
    else b.Huffman(0xC0 + (v - 280), 8);
}
static void Repeticion(Bits& b, int largo, int dist) {
    static const int baseL[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const int extraL[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const int baseD[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const int extraD[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    int i = 28;
    while (baseL[i] > largo) i--;
    Literal(b, (unsigned)(257 + i));
    if (extraL[i]) b.Poner((unsigned)(largo - baseL[i]), extraL[i]);
    int j = 29;
    while (baseD[j] > dist) j--;
    b.Huffman((unsigned)j, 5);
    if (extraD[j]) b.Poner((unsigned)(dist - baseD[j]), extraD[j]);
}
static std::string Deflate(const std::string& d) {
    const int n = (int)d.size();
    const unsigned char* s = (const unsigned char*)d.data();
    const int kVentana = 32768, kHash = 1 << 15;
    std::vector<int> cabeza(kHash, -1), previo(kVentana, -1);
    Bits b;
    b.Poner(1, 1);   // ultimo bloque
    b.Poner(1, 2);   // codigos fijos
    int i = 0;
    while (i < n) {
        int mejor = 0, mejorD = 0;
        if (i + 3 <= n) {
            const unsigned h = ((unsigned)s[i] * 2654435761u ^ (unsigned)s[i + 1] * 40503u ^ (unsigned)s[i + 2]) & (kHash - 1);
            int c = cabeza[h];
            for (int vueltas = 0; c >= 0 && i - c <= kVentana && vueltas < 32; vueltas++) {
                int l = 0;
                const int tope = (n - i < 258) ? n - i : 258;
                while (l < tope && s[c + l] == s[i + l]) l++;
                if (l > mejor) { mejor = l; mejorD = i - c; if (l == tope) break; }
                c = previo[c % kVentana];
            }
            previo[i % kVentana] = cabeza[h];
            cabeza[h] = i;
        }
        if (mejor >= 3) {
            Repeticion(b, mejor, mejorD);
            for (int k = 1; k < mejor; k++) {   // (los saltados tambien entran a la busqueda)
                const int q = i + k;
                if (q + 3 > n) break;
                const unsigned h = ((unsigned)s[q] * 2654435761u ^ (unsigned)s[q + 1] * 40503u ^ (unsigned)s[q + 2]) & (kHash - 1);
                previo[q % kVentana] = cabeza[h];
                cabeza[h] = q;
            }
            i += mejor;
        } else Literal(b, s[i++]);
    }
    Literal(b, 256);   // fin del bloque
    b.Cerrar();
    return b.out;
}
static unsigned Crc32(const std::string& d, unsigned crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < d.size(); i++) {
        crc ^= (unsigned char)d[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}
static void Be32(std::string& o, unsigned v) { o += (char)(v >> 24); o += (char)(v >> 16); o += (char)(v >> 8); o += (char)v; }
static void Trozo(std::string& png, const char* tipo, const std::string& datos) {
    Be32(png, (unsigned)datos.size());
    const std::string td = std::string(tipo, 4) + datos;
    png += td;
    Be32(png, Crc32(td) ^ 0xFFFFFFFFu);
}
// RGB (filas de arriba hacia abajo) -> PNG. Cada fila con el filtro que deja menos (Sub o Up: la resta con el vecino
// convierte las zonas lisas en ceros, que el deflate junta)
static std::string PngComprimido(const std::vector<unsigned char>& rgb, int w, int h) {
    std::string crudo;
    crudo.reserve((size_t)(w * 3 + 1) * h);
    const int fila = w * 3;
    std::string sub((size_t)fila, 0), up((size_t)fila, 0);
    for (int y = 0; y < h; y++) {
        const unsigned char* f = &rgb[(size_t)y * fila];
        const unsigned char* a = y > 0 ? f - fila : NULL;
        long costoSub = 0, costoUp = 0;
        for (int x = 0; x < fila; x++) {
            const unsigned char vs = (unsigned char)(f[x] - (x >= 3 ? f[x - 3] : 0));
            const unsigned char vu = (unsigned char)(f[x] - (a ? a[x] : 0));
            sub[x] = (char)vs; up[x] = (char)vu;
            costoSub += vs < 128 ? vs : 256 - vs;
            costoUp += vu < 128 ? vu : 256 - vu;
        }
        if (costoUp < costoSub) { crudo += (char)2; crudo += up; } else { crudo += (char)1; crudo += sub; }
    }
    std::string z;
    z += (char)0x78; z += (char)0x01;   // (zlib: deflate, ventana de 32 KB)
    z += Deflate(crudo);
    unsigned a1 = 1, b1 = 0;
    for (size_t i = 0; i < crudo.size(); i++) { a1 = (a1 + (unsigned char)crudo[i]) % 65521u; b1 = (b1 + a1) % 65521u; }
    Be32(z, (b1 << 16) | a1);
    std::string png("\x89PNG\r\n\x1a\n", 8), ihdr;
    Be32(ihdr, (unsigned)w); Be32(ihdr, (unsigned)h);
    ihdr += (char)8; ihdr += (char)2; ihdr += (char)0; ihdr += (char)0; ihdr += (char)0;   // 8 bits, RGB
    Trozo(png, "IHDR", ihdr);
    Trozo(png, "IDAT", z);
    Trozo(png, "IEND", std::string());
    return png;
}

static void Enviar(const std::string& json) {
    fputs(json.c_str(), gProto);
    fputc('\n', gProto);
    fflush(gProto);
}

// ============================================================================
//  Las herramientas
// ============================================================================
struct Resultado {
    std::string texto;
    std::string imagenPng;   // (la captura)
    bool error;
    Resultado() : error(false) {}
};

static Object* Buscar(Object* o, const std::string& nombre) {
    if (!o) return NULL;
    if (o != SceneCollection && o->name == nombre) return o;
    for (size_t i = 0; i < o->Childrens.size(); i++) if (Object* r = Buscar(o->Childrens[i], nombre)) return r;
    return NULL;
}
// LAS COORDENADAS DE LA PERSONA: las del panel Propiedades, con Z para arriba. Adentro el editor tiene Y para arriba:
// el campo "Y" de la posicion es pos.z y el "Z" es pos.y. La rotacion (euler en grados) y la escala van como el panel
static void Arbol(Object* o, int prof, std::string& out) {
    char b[512];
    const Vector3& p = o->pos; const Vector3& r = o->rotEuler; const Vector3& e = o->scale;
    snprintf(b, sizeof(b), "%*s- %s (%s) pos %.3g %.3g %.3g | rot %.3g %.3g %.3g | escala %.3g %.3g %.3g%s%s%s\n",
             prof * 2, "", o->name.c_str(), W3dNombreTipo(o->getType()), p.x, p.z, p.y, r.x, r.y, r.z, e.x, e.y, e.z,
             o->select ? " [seleccionado]" : "", o == ObjActivo ? " [activo]" : "", o->visible ? "" : " [oculto]");
    out += b;
    for (size_t i = 0; i < o->Childrens.size(); i++) Arbol(o->Childrens[i], prof + 1, out);
}

static Resultado HerrEscena() {
    Resultado r;
    if (!SceneCollection) { r.error = true; r.texto = "no hay escena"; return r; }
    r.texto = "Escena (como el panel Propiedades: Z para arriba, rotacion euler en grados):\n";
    for (size_t i = 0; i < SceneCollection->Childrens.size(); i++) Arbol(SceneCollection->Childrens[i], 0, r.texto);
    if (SceneCollection->Childrens.empty()) r.texto += "(vacia)\n";
    return r;
}

// la pantalla tal como se ve (o solo la vista 3D activa) a PNG: el mismo dibujo que la captura del harness (uishot),
// achicada (de a bloques enteros, hasta 'anchoMax') y comprimida
static Resultado HerrCaptura(bool soloVista3D, int anchoMax) {
    Resultado r;
    if (!rootViewport) { r.error = true; r.texto = "no hay layout de viewports"; return r; }
    { extern void CargarTexturasPendientes(); CargarTexturasPendientes(); }
    const int W = MenuPantallaW, H = W3dPantallaAlto;
    if (W < 1 || H < 1) { r.error = true; r.texto = "la ventana no tiene tamano"; return r; }
    rootViewport->Render();
    LayoutRenderMenu(W, H);   // (los menus y popups van aparte, encima de todo)
    w3dEngine::Finish();
    int x0 = 0, y0 = 0, w = W, h = H;
    Viewport3D* vp = Viewport3DActive;
    if (soloVista3D && vp && vp->width > 0 && vp->height > 0) { x0 = vp->x; w = vp->width; h = vp->height; y0 = H - vp->y - vp->height; }
    std::vector<unsigned char> px((size_t)w * h * 4, 0);
    w3dEngine::ReadPixelsRGBA(x0, y0, w, h, &px[0]);   // (de abajo hacia arriba)
    if (anchoMax < 64) anchoMax = 64;
    const int f = (w + anchoMax - 1) / anchoMax;      // de a cuantos pixeles se promedia
    const int ow = w / f, oh = h / f;
    std::vector<unsigned char> rgb((size_t)ow * oh * 3);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++)
            for (int c = 0; c < 3; c++) {
                unsigned suma = 0;
                for (int j = 0; j < f; j++)
                    for (int i = 0; i < f; i++) suma += px[((size_t)(h - 1 - (y * f + j)) * w + (x * f + i)) * 4 + c];
                rgb[((size_t)y * ow + x) * 3 + c] = (unsigned char)(suma / (unsigned)(f * f));
            }
    r.imagenPng = PngComprimido(rgb, ow, oh);
    char b[128];
    snprintf(b, sizeof(b), "%s: %dx%d (la ventana mide %dx%d)", soloVista3D ? "vista 3D" : "ventana", ow, oh, W, H);
    r.texto = b;
    return r;
}

static Resultado HerrCaptureFile(const JVal* args) {
    Resultado r;
    const std::string path = Texto(Campo(args, "path"));
    if (path.empty()) { r.error = true; r.texto = "capture_file requires path"; return r; }

    const std::string zona = Texto(Campo(args, "zona"));
    const bool soloVista3D = (zona != "ventana");
    const JVal* anchoArg = Campo(args, "ancho_max");
    const int anchoMax = (anchoArg && anchoArg->tipo == 1) ? (int)anchoArg->num : 800;
    r = HerrCaptura(soloVista3D, anchoMax);
    if (r.error) return r;

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { r.error = true; r.texto = "cannot open capture path: " + path; r.imagenPng.clear(); return r; }
    const size_t n = fwrite(r.imagenPng.data(), 1, r.imagenPng.size(), f);
    const int closeRc = fclose(f);
    if (n != r.imagenPng.size() || closeRc != 0) {
        r.error = true;
        r.texto = "failed writing capture: " + path;
        r.imagenPng.clear();
        return r;
    }

    const size_t bytes = r.imagenPng.size();
    const std::string dims = r.texto;
    r.imagenPng.clear(); // modo archivo: no enviar base64 por el protocolo
    r.texto = "capture_file path=" + path + "\n" + dims +
              "\nbytes=" + std::to_string(bytes);
    return r;
}

// comandos del harness, uno por linea: corta en el primero que falla. Devuelve lo que imprimieron
static Resultado HerrComandos(const std::string& texto) {
    Resultado r;
    LeerCaptura();   // (lo que haya quedado de antes no es de estos comandos)
    size_t a = 0;
    int n = 0;
    while (a <= texto.size()) {
        size_t b = texto.find('\n', a);
        if (b == std::string::npos) b = texto.size();
        std::string linea = texto.substr(a, b - a);
        a = b + 1;
        const size_t i0 = linea.find_first_not_of(" \t\r");
        if (i0 == std::string::npos) continue;
        const size_t i1 = linea.find_last_not_of(" \t\r");
        linea = linea.substr(i0, i1 - i0 + 1);
        if (linea[0] == '#') continue;
        std::string err;
        const bool ok = W3dRunCommand(linea, err);
        const std::string impreso = LeerCaptura();
        r.texto += (ok ? "OK    " : "FALLO ") + linea + "\n" + impreso;
        if (!ok) { r.texto += "  -> " + err + "\n"; r.error = true; break; }
        n++;
    }
    if (r.texto.empty()) r.texto = "(sin comandos)";
    g_redraw = true;
    return r;
}

static Resultado HerrAgregar(const std::string& tipo) {
    Resultado r;
    struct { const char* nombre; void (*fn)(); } tabla[] = {
        { "cube", AddCube }, { "plane", AddPlane }, { "circle", AddCircle }, { "uvsphere", AddUVSphere },
        { "cone", AddCone }, { "cylinder", AddCylinder }, { "vertex", AddVertex }, { "empty", AddEmpty },
        { "camera", AddCamera }, { "light", AddLight },
    };
    for (size_t i = 0; i < sizeof(tabla) / sizeof(tabla[0]); i++) {
        if (tipo != tabla[i].nombre) continue;
        LeerCaptura();
        tabla[i].fn();
        LeerCaptura();
        g_redraw = true;
        r.texto = std::string("agregado: ") + (ObjActivo ? ObjActivo->name : tipo);
        return r;
    }
    r.error = true;
    r.texto = "tipo desconocido: '" + tipo + "' (cube, plane, circle, uvsphere, cone, cylinder, vertex, empty, camera, light)";
    return r;
}

static Resultado HerrSeleccionar(const std::string& nombre, bool agregar) {
    Resultado r;
    Object* o = nombre.empty() ? NULL : Buscar(SceneCollection, nombre);
    if (!nombre.empty() && !o) { r.error = true; r.texto = "no hay ningun objeto llamado '" + nombre + "'"; return r; }
    UndoCapturarSeleccion();
    if (!agregar) DeseleccionarTodo();
    if (o) o->Seleccionar();
    g_redraw = true;
    r.texto = o ? "seleccionado: " + o->name + " (activo)" : std::string("nada seleccionado");
    return r;
}

static Resultado HerrTransformar(const JVal* args) {
    Resultado r;
    const std::string nombre = Texto(Campo(args, "nombre"));
    Object* o = nombre.empty() ? ObjActivo : Buscar(SceneCollection, nombre);
    if (!o) { r.error = true; r.texto = nombre.empty() ? "no hay objeto activo" : "no hay ningun objeto llamado '" + nombre + "'"; return r; }
    float p[3], rot[3], e[3];
    const bool hayP = Tres(Campo(args, "posicion"), p), hayR = Tres(Campo(args, "rotacion"), rot), hayE = Tres(Campo(args, "escala"), e);
    if (!hayP && !hayR && !hayE) { r.error = true; r.texto = "falta posicion, rotacion o escala (cada una, una lista de 3 numeros)"; return r; }
    UndoTransformIniciarObj(o);   // (queda en el Ctrl+Z como un transform hecho a mano)
    if (hayP) o->pos = Vector3(p[0], p[2], p[1]);   // (Z para arriba, como el panel: ver Arbol)
    if (hayR) o->SetRotEuler(Vector3(rot[0], rot[1], rot[2]));
    if (hayE) o->scale = Vector3(e[0], e[1], e[2]);
    UndoTransformConfirmar();
    { extern bool g_objetosMovidos; g_objetosMovidos = true; }
    g_redraw = true;
    std::string t;
    Arbol(o, 0, t);
    r.texto = "listo:\n" + t;
    return r;
}

static Resultado HerrObjectCreate(const JVal* args) {
    const std::string tipo = Texto(Campo(args, "tipo"));
    Resultado r = HerrAgregar(tipo);
    if (r.error) return r;

    std::string out = r.texto + "\n";
    const std::string nombre = Texto(Campo(args, "nombre"));
    if (!nombre.empty()) {
        Resultado rn = HerrComandos("objname " + nombre);
        out += rn.texto;
        if (rn.error) { rn.texto = out; return rn; }
    }

    if (Campo(args, "posicion") || Campo(args, "rotacion") || Campo(args, "escala")) {
        Resultado rt = HerrTransformar(args);
        out += rt.texto;
        if (rt.error) { rt.texto = out; return rt; }
    } else if (ObjActivo) {
        std::string tree;
        Arbol(ObjActivo, 0, tree);
        out += "final:\n" + tree;
    }

    r.texto = out;
    return r;
}

static Resultado HerrParentKeep(const JVal* args) {
    Resultado r;
    const std::string childName = Texto(Campo(args, "child"));
    const std::string parentName = Texto(Campo(args, "parent"));
    Object* child = childName.empty() ? NULL : Buscar(SceneCollection, childName);
    Object* parent = parentName.empty() ? NULL : Buscar(SceneCollection, parentName);
    if (!child) { r.error = true; r.texto = "child not found: " + childName; return r; }
    if (!parent) { r.error = true; r.texto = "parent not found: " + parentName; return r; }

    ReparentKeepTransform(child, parent);
    if (child->Parent != parent) {
        r.error = true;
        r.texto = "parent_keep failed: " + childName + " -> " + parentName;
        return r;
    }

    std::string tree;
    Arbol(child, 0, tree);
    r.texto = "parent_keep " + childName + " -> " + parentName + "\n" + tree;
    g_redraw = true;
    return r;
}

static Resultado HerrImportar(const std::string& ruta) {
    Resultado r;
    if (ruta.empty()) { r.error = true; r.texto = "falta la ruta"; return r; }
    LeerCaptura();
    const bool ok = ImportModeloPorExtension(ruta);
    r.texto = LeerCaptura();
    r.texto += ok ? "importado: " + ruta : "no se pudo importar: " + ruta;
    r.error = !ok;
    g_redraw = true;
    return r;
}

static bool BoolArg(const JVal* v, bool defecto = false) {
    return v && v->tipo == 3 ? v->b : defecto;
}
static int IntArg(const JVal* v, int defecto) {
    return v && v->tipo == 1 ? (int)v->num : defecto;
}
static std::string Minusculas(std::string s) {
    for (size_t i = 0; i < s.size(); i++) s[i] = (char)tolower((unsigned char)s[i]);
    return s;
}
static std::vector<std::string> TerminosBusqueda(const std::string& consulta) {
    std::vector<std::string> out;
    const std::string q = Minusculas(consulta);
    size_t p = 0;
    while (p < q.size()) {
        while (p < q.size() && (q[p] == ' ' || q[p] == '	' || q[p] == ',' || q[p] == '/' || q[p] == '-')) p++;
        if (p >= q.size()) break;
        size_t e = p;
        while (e < q.size() && q[e] != ' ' && q[e] != '	' && q[e] != ',' && q[e] != '/' && q[e] != '-') e++;
        const std::string t = q.substr(p, e - p);
        if (!t.empty() && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
        p = e;
    }
    return out;
}

static int PuntajeBusqueda(const std::string& consulta,
                           const std::string& principal,
                           const std::string& texto) {
    if (consulta.empty()) return 1;
    const std::string q = Minusculas(consulta);
    const std::string p = Minusculas(principal);
    const std::string h = Minusculas(texto);
    const std::vector<std::string> terminos = TerminosBusqueda(consulta);

    int score = 0;
    if (p == q) score += 2000;
    else if (p.find(q) != std::string::npos) score += 1000;
    else if (h.find(q) != std::string::npos) score += 500;

    for (size_t i = 0; i < terminos.size(); i++) {
        const std::string& t = terminos[i];
        if (p == t) score += 300;
        else if (p.find(t) == 0) score += 220;
        else if (p.find(t) != std::string::npos) score += 160;
        else if (h.find(t) != std::string::npos) score += 60;
        else if (t.size() >= 4) {
            const std::string stem = t.substr(0, 3);
            if (p.find(stem) != std::string::npos) score += 40;
            else if (h.find(stem) != std::string::npos) score += 15;
        }
    }
    return score;
}

static const char* const kHerramientasNativas[] = {
    "guide", "state", "catalog_index", "catalog_search", "catalog_describe",
    "workspace_status", "session_open", "session_plan", "session_switch", "session_close",
    "task_open", "task_switch", "task_suspend", "task_resume", "task_commit", "task_close",
    "session_test", "menu_execute", "shortcut", "input",
    "escena", "captura", "capture_file", "object_create", "parent_keep", "agregar_objeto", "seleccionar", "transformar",
    "borrar_seleccion", "deshacer", "abrir_proyecto", "guardar_proyecto",
    "importar_modelo", "command", "comando"
};
static const unsigned kHerramientasNativasN =
    (unsigned)(sizeof(kHerramientasNativas) / sizeof(kHerramientasNativas[0]));

static const char* ModoNombre(int m) {
    if (m == ObjectMode) return "object";
    if (m == EditMode) return "edit";
    if (m == PoseMode) return "pose";
    if (m == WeightPaint) return "weight_paint";
    if (m == VertexPaint) return "vertex_paint";
    if (m == TexturePaint) return "texture_paint";
    return "other";
}

static PopupMenu* MenuPadreRegistrado(PopupMenu* hijo, MenuItem** itemPadre = NULL) {
    if (itemPadre) *itemPadre = NULL;
    if (!hijo) return NULL;
    for (int m = 0; m < W3dMenuRegistroCantidad(); m++) {
        PopupMenu* p = W3dMenuRegistro(m);
        if (!p) continue;
        for (size_t i = 0; i < p->items.size(); i++) {
            if (p->items[i] && p->items[i]->submenu == hijo) {
                if (itemPadre) *itemPadre = p->items[i];
                return p;
            }
        }
    }
    return NULL;
}
static std::string MenuRuta(PopupMenu* menu, const MenuItem* item) {
    std::vector<std::string> partes;
    PopupMenu* actual = menu;
    int guard = 0;
    while (actual && guard++ < 32) {
        MenuItem* desdePadre = NULL;
        PopupMenu* padre = MenuPadreRegistrado(actual, &desdePadre);
        if (desdePadre && !desdePadre->text.empty()) partes.push_back(desdePadre->text);
        else if (!actual->titulo.empty()) partes.push_back(actual->titulo);
        actual = padre;
    }
    std::reverse(partes.begin(), partes.end());
    std::string ruta;
    for (size_t i = 0; i < partes.size(); i++) {
        if (!ruta.empty()) ruta += " > ";
        ruta += partes[i];
    }
    if (item) {
        if (!ruta.empty()) ruta += " > ";
        ruta += item->text;
    }
    return ruta;
}
static int MenuItemsVivos() {
    int n = 0;
    for (int m = 0; m < W3dMenuRegistroCantidad(); m++) {
        PopupMenu* menu = W3dMenuRegistro(m);
        if (menu) n += (int)menu->items.size();
    }
    return n;
}
static bool ParseMenuRef(const std::string& ref, int& menuIndex, int& itemIndex) {
    menuIndex = itemIndex = -1;
    if (ref.compare(0, 5, "menu:") != 0) return false;
    const size_t p = ref.find(':', 5);
    if (p == std::string::npos) return false;
    const std::string a = ref.substr(5, p - 5);
    const std::string b = ref.substr(p + 1);
    if (a.empty() || b.empty()) return false;
    char* e1 = NULL; char* e2 = NULL;
    const long m = strtol(a.c_str(), &e1, 10);
    const long i = strtol(b.c_str(), &e2, 10);
    if (!e1 || *e1 || !e2 || *e2 || m < 0 || i < 0) return false;
    menuIndex = (int)m; itemIndex = (int)i;
    return true;
}
static Resultado HerrShortcut(const std::string& id);

static Resultado HerrMenuExecute(const JVal* args) {
    Resultado r;
    const std::string ref = Texto(Campo(args, "ref"));
    int mi = -1, ii = -1;
    if (!ParseMenuRef(ref, mi, ii)) {
        r.error = true; r.texto = "invalid menu ref; use catalog_search type=menu"; return r;
    }
    PopupMenu* menu = W3dMenuRegistro(mi);
    if (!menu || ii < 0 || ii >= (int)menu->items.size() || !menu->items[(size_t)ii]) {
        r.error = true; r.texto = "menu ref is stale; search the catalog again"; return r;
    }
    MenuItem* it = menu->items[(size_t)ii];
    if (it->gris && !*it->gris) {
        r.error = true; r.texto = "menu item is disabled: " + MenuRuta(menu, it); return r;
    }
    if (it->submenu) {
        r.error = true;
        r.texto = "menu item opens a submenu; search for a terminal child: " + MenuRuta(menu, it);
        return r;
    }

    bool ok = false;
    if (it->checkbox) {
        *it->checkbox = !*it->checkbox;
        if (W3dMenuElegidoHook) W3dMenuElegidoHook(menu, it);
        ok = menu->Ejecutar(it->id);
        if (!ok) ok = true; // en muchos menus, cambiar el checkbox ya es la accion
    } else if (it->valorFloat) {
        const JVal* vv = Campo(args, "value");
        if (!vv || vv->tipo != 1) {
            r.error = true; r.texto = "slider menu item requires numeric value"; return r;
        }
        float v = (float)vv->num;
        if (v < it->vmin) v = it->vmin;
        if (v > it->vmax) v = it->vmax;
        *it->valorFloat = v;
        ok = menu->Ejecutar(it->id);
        if (!ok) ok = true;
    } else {
        if (W3dMenuElegidoHook) W3dMenuElegidoHook(menu, it);
        ok = menu->Ejecutar(it->id);
        if (!ok) {
            PopupMenu* padre = MenuPadreRegistrado(menu);
            if (padre && padre->action) { padre->action(it->id); ok = true; }
        }
    }
    if (!ok && !it->atajo.empty() && it->atajo[0] == '@') {
        Resultado viaShortcut = HerrShortcut(it->atajo.substr(1));
        if (!viaShortcut.error) {
            viaShortcut.texto = "menu executed via shortcut: " + MenuRuta(menu, it) + "\n" + viaShortcut.texto;
            return viaShortcut;
        }
    }
    if (!ok) {
        r.error = true;
        r.texto = "menu item has no direct semantic dispatcher; use its cataloged shortcut or generic input: " +
                  MenuRuta(menu, it);
        return r;
    }
    g_redraw = true;
    r.texto = "menu executed: " + MenuRuta(menu, it);
    if (it->checkbox) r.texto += std::string(" = ") + (*it->checkbox ? "true" : "false");
    if (it->valorFloat) {
        char b[64]; snprintf(b, sizeof(b), " = %.6g", *it->valorFloat); r.texto += b;
    }
    return r;
}

static std::vector<std::string> ListaTexto(const JVal* v) {
    std::vector<std::string> out;
    if (!v || v->tipo != 5) return out;
    for (size_t i = 0; i < v->lista.size(); i++) {
        if (v->lista[i] && v->lista[i]->tipo == 2 && !v->lista[i]->str.empty())
            out.push_back(v->lista[i]->str);
    }
    return out;
}

static Resultado DesdeWorkspace(const W3dLlmWorkspaceResult& w) {
    Resultado r;
    r.error = !w.ok;
    r.texto = w.text;
    return r;
}

static bool HerramientaMuta(const std::string& nombre) {
    return nombre == "menu_execute" || nombre == "shortcut" || nombre == "input" ||
           nombre == "object_create" || nombre == "parent_keep" || nombre == "agregar_objeto" || nombre == "seleccionar" || nombre == "transformar" ||
           nombre == "borrar_seleccion" || nombre == "deshacer" ||
           nombre == "abrir_proyecto" || nombre == "guardar_proyecto" ||
           nombre == "importar_modelo" || nombre == "command" || nombre == "comando";
}

static std::string DetalleCambio(const std::string& nombre, const JVal* args,
                                 const Resultado& r) {
    std::string d;
    if (nombre == "command") d = Texto(Campo(args, "commands"));
    else if (nombre == "comando") d = Texto(Campo(args, "comandos"));
    else if (nombre == "shortcut") d = Texto(Campo(args, "id"));
    else if (nombre == "menu_execute") d = Texto(Campo(args, "ref"));
    else if (nombre == "input") {
        d = Texto(Campo(args, "tipo"));
        const std::string v = Texto(Campo(args, "valor"));
        if (!v.empty()) d += " " + v;
    } else d = r.texto;
    if (d.size() > 1400) d.resize(1400);
    const std::string project = w3dPath.empty() ? "(new/unsaved)" : w3dPath;
    return "project=" + project + " | " + d;
}

static Resultado HerrSessionTest(const JVal* args) {
    Resultado r;
    const std::string commands = Texto(Campo(args, "commands"));
    const std::string expected = Texto(Campo(args, "expect_contains"));
    if (commands.empty()) {
        r.error = true;
        r.texto = "session_test requires commands";
        return r;
    }

    const Resultado executed = HerrComandos(commands);
    const bool passed = !executed.error &&
                        (expected.empty() || executed.texto.find(expected) != std::string::npos);
    const W3dLlmWorkspaceResult saved =
        W3dLlmWorkspaceRecordTest(commands, expected, executed.texto, passed);
    r.error = !saved.ok || !passed;
    r.texto = saved.text + "\n" + executed.texto;
    if (!expected.empty() && executed.texto.find(expected) == std::string::npos)
        r.texto += "\nexpected substring not found: " + expected;
    return r;
}

static Resultado HerrCatalogIndex() {
    Resultado r;
    std::map<char, unsigned> commands;
    for (unsigned i = 0; i < kW3dGeneratedCommandCount; i++) {
        char c = kW3dGeneratedCommands[i].name[0];
        c = (char)toupper((unsigned char)c);
        if (c < 'A' || c > 'Z') c = '#';
        commands[c]++;
    }

    std::map<std::string, unsigned> shortcuts;
    for (int i = 0; i < W3dAtajosCantidad(); i++)
        shortcuts[W3dAtajoCtxNombre(W3dAtajoInfo(i).ctx)]++;

    std::map<std::string, unsigned> menus;
    for (int m = 0; m < W3dMenuRegistroCantidad(); m++) {
        PopupMenu* menu = W3dMenuRegistro(m);
        if (!menu) continue;
        for (size_t i = 0; i < menu->items.size(); i++) {
            if (!menu->items[i]) continue;
            std::string path = MenuRuta(menu, menu->items[i]);
            const size_t p = path.find(" > ");
            const std::string root = p == std::string::npos ? path : path.substr(0, p);
            menus[root.empty() ? "(untitled)" : root]++;
        }
    }

    std::ostringstream o;
    o << "FUNCTION/CAPABILITY INDEX\n";
    o << "commands=" << kW3dGeneratedCommandCount << " alphabetic:";
    for (std::map<char, unsigned>::const_iterator it = commands.begin(); it != commands.end(); ++it)
        o << " " << it->first << ":" << it->second;
    o << "\nshortcuts=" << W3dAtajosCantidad() << " contexts:";
    for (std::map<std::string, unsigned>::const_iterator it = shortcuts.begin(); it != shortcuts.end(); ++it)
        o << " " << it->first << ":" << it->second;
    o << "\nmenus=" << W3dMenuRegistroCantidad() << " roots:";
    for (std::map<std::string, unsigned>::const_iterator it = menus.begin(); it != menus.end(); ++it)
        o << "\n  " << it->first << ": " << it->second;
    o << "\nnative_tools=" << kHerramientasNativasN
      << "\nUse catalog_search to drill into any index; limit=0 returns every match.\n";
    r.texto = o.str();
    return r;
}

static Resultado HerrGuia(const JVal* args) {
    Resultado r;
    const int level = IntArg(Campo(args, "level"), 0);
    std::ostringstream o;

    o << "Whisk3D LLM quick guide (level " << level << ")\n"
      << "1. workspace_status: inspect the current session/task context.\n"
      << "2. session_open(title, objective), then session_plan(points[]).\n"
      << "3. task_open(title, point_id) before any editor mutation.\n"
      << "4. catalog_index/search/describe -> command, shortcut, menu_execute or input.\n"
      << "5. task_commit(summary, comment) checkpoints the task; task_close resolves its plan point.\n"
      << "6. session_test runs a real editor test; session_close derives objective status automatically.\n";

    if (level >= 1) {
        o << "\nSESSIONS/TASKS\n"
          << "- At most 3 active sessions; each session owns exactly one objective.\n"
          << "- Every objective has an incremental plan. Each task is linked to one plan point.\n"
          << "- At most 8 active/suspended tasks per session; closed tasks do not count.\n"
          << "- task_suspend keeps work pending; task_resume or task_switch changes focus.\n"
          << "- Every task must have an internal task_commit with summary+comment before close.\n"
          << "- task_close outcome is complete, partial or missing and updates its linked plan point.\n"
          << "- session_close derives confirmed/partial/missing from the plan; confirmed also requires the latest session_test to pass.\n"
          << "- Context reminders are periodic, and immediate after errors or session/task operations.\n";
    }

    if (level >= 2) {
        o << "\nCAPABILITY DISCOVERY\n"
          << "- commands=" << kW3dGeneratedCommandCount
          << ", shortcuts=" << W3dAtajosCantidad()
          << ", live_menu_items=" << MenuItemsVivos() << ".\n"
          << "- catalog_index gives coarse indexes; catalog_search drills down by terms/type.\n"
          << "- catalog_search limit=0 returns every match; there is no capability whitelist.\n"
          << "- catalog_describe returns exact command examples, shortcut bindings or menu metadata.\n"
          << "- command is the universal semantic dispatcher; shortcut/menu/input cover native UI paths.\n"
          << "- capture verifies visual results; scene/state are read-only inspection tools.\n";
    }

    if (level >= 3) {
        o << "\nPERSISTENCE/AUDIT\n"
          << "- Workspace state lives under GetUserDataDir()/llm-workspace.\n"
          << "- registry.tsv is the global session index.\n"
          << "- each session stores session.tsv, plan.tsv, tasks.tsv, tests.tsv and events.tsv.\n"
          << "- each task stores task.tsv, changes.tsv and commits.tsv.\n"
          << "- successful mutating tools are appended automatically to the current task change index.\n"
          << "- session_test records actual command output; the agent cannot self-declare a passing test.\n"
          << "- workspace_status(verbose=true) exposes the persistence path and complete closed-task context.\n";
    }

    r.texto = o.str();
    return r;
}

static Resultado HerrStato() {
    Resultado r;
    int seleccionados = 0;
    if (SceneCollection) {
        std::vector<Object*> pila;
        pila.push_back(SceneCollection);
        while (!pila.empty()) {
            Object* o = pila.back();
            pila.pop_back();
            if (o != SceneCollection && o->select) seleccionados++;
            for (size_t i = 0; i < o->Childrens.size(); i++) pila.push_back(o->Childrens[i]);
        }
    }
    char b[1024];
    snprintf(b, sizeof(b),
             "project=%s\nunsaved=%s\nmode=%s\nactive=%s\nselected=%d\nviewport=%d\n"
             "commands=%u\nshortcuts=%d\nmenus=%d\nmenu_items=%d\n",
             w3dPath.empty() ? "(new/unsaved)" : w3dPath.c_str(),
             W3dCambiosHayCache() ? "true" : "false",
             ModoNombre(InteractionMode),
             ObjActivo ? ObjActivo->name.c_str() : "(none)",
             seleccionados,
             viewPortActive ? viewPortActive->ViewportKind() : -1,
             kW3dGeneratedCommandCount,
             W3dAtajosCantidad(),
             W3dMenuRegistroCantidad(),
             MenuItemsVivos());
    r.texto = b;
    return r;
}

static bool TipoCatalogo(const std::string& pedido, const char* tipo) {
    return pedido.empty() || pedido == "all" || pedido == tipo;
}

static Resultado HerrCatalogoBuscar(const JVal* args) {
    Resultado r;
    std::string consulta = Texto(Campo(args, "query"));
    if (consulta.empty()) consulta = Texto(Campo(args, "consulta"));
    std::string tipo = Minusculas(Texto(Campo(args, "type")));
    if (tipo.empty()) tipo = Minusculas(Texto(Campo(args, "tipo")));
    const JVal* limitArg = Campo(args, "limit");
    if (!limitArg) limitArg = Campo(args, "limite");
    int limite = IntArg(limitArg, 40);
    int offset = IntArg(Campo(args, "offset"), 0);
    if (limite < 0) limite = 0;
    if (offset < 0) offset = 0;

    struct Hit {
        int score;
        std::string line;
    };
    std::vector<Hit> hits;

    const auto agregar = [&](const std::string& principal,
                             const std::string& searchable,
                             const std::string& line) {
        const int score = PuntajeBusqueda(consulta, principal, searchable);
        if (score > 0) hits.push_back(Hit{score, line});
    };

    if (TipoCatalogo(tipo, "command")) {
        for (unsigned i = 0; i < kW3dGeneratedCommandCount; i++) {
            const W3dGeneratedCommandInfo& c = kW3dGeneratedCommands[i];
            std::string hay = c.name;
            if (*c.example1) hay += std::string(" ") + c.example1;
            if (*c.example2) hay += std::string(" ") + c.example2;
            std::string linea = std::string("command ") + c.name;
            if (*c.example1) linea += std::string(" | ex: ") + c.example1;
            agregar(c.name, hay, linea);
        }
    }

    if (TipoCatalogo(tipo, "shortcut")) {
        for (int i = 0; i < W3dAtajosCantidad(); i++) {
            const W3dAtajoDef& a = W3dAtajoInfo(i);
            const std::string actual = W3dAtajoTexto(i);
            const std::string ctx = W3dAtajoCtxNombre(a.ctx);
            const std::string principal = std::string(a.id) + " " + a.nombre;
            const std::string hay = principal + " " + ctx + " " + actual + " " + a.defecto;
            agregar(principal, hay,
                    std::string("shortcut ") + a.id + " | " + a.nombre +
                    " | context=" + ctx + " | current=" +
                    (actual.empty() ? "(none)" : actual));
        }
    }

    if (TipoCatalogo(tipo, "menu")) {
        for (int m = 0; m < W3dMenuRegistroCantidad(); m++) {
            PopupMenu* menu = W3dMenuRegistro(m);
            if (!menu) continue;
            for (size_t i = 0; i < menu->items.size(); i++) {
                MenuItem* it = menu->items[i];
                if (!it) continue;
                const std::string path = MenuRuta(menu, it);
                const std::string shortcut = MenuAtajoVisible(it);
                const std::string hay = path + " " + shortcut;
                char ref[64]; snprintf(ref, sizeof(ref), "menu:%d:%d", m, (int)i);
                std::string linea = std::string("menu ") + ref + " | " + path;
                if (!shortcut.empty()) linea += " | shortcut=" + shortcut;
                if (it->submenu) linea += " | submenu";
                else if (it->checkbox) linea += std::string(" | checkbox=") + (*it->checkbox ? "true" : "false");
                else if (it->valorFloat) {
                    char b[96];
                    snprintf(b, sizeof(b), " | slider=%.6g [%.6g..%.6g]", *it->valorFloat, it->vmin, it->vmax);
                    linea += b;
                } else linea += " | action";
                if (it->gris && !*it->gris) linea += " | disabled";
                agregar(path, hay, linea);
            }
        }
    }

    if (TipoCatalogo(tipo, "tool")) {
        for (unsigned i = 0; i < kHerramientasNativasN; i++) {
            const std::string nombre = kHerramientasNativas[i];
            agregar(nombre, nombre, "tool " + nombre);
        }
    }

    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit& a, const Hit& b) {
                         if (a.score != b.score) return a.score > b.score;
                         return a.line < b.line;
                     });

    const int total = (int)hits.size();
    const bool sinLimite = (limite == 0);
    const int fin = sinLimite ? total : std::min(total, offset + limite);
    std::string out;
    int emitidos = 0;
    for (int i = offset; i < fin; i++) {
        out += hits[(size_t)i].line;
        out += '\n';
        emitidos++;
    }

    char cab[256];
    snprintf(cab, sizeof(cab), "matches=%d returned=%d offset=%d limit=%s\n",
             total, emitidos, offset, sinLimite ? "all" : std::to_string(limite).c_str());
    r.texto = cab + out;
    return r;
}

static Resultado HerrCatalogoDescrivere(const std::string& nombre) {
    Resultado r;
    if (nombre.empty()) { r.error = true; r.texto = "falta nombre"; return r; }

    for (unsigned i = 0; i < kW3dGeneratedCommandCount; i++) {
        const W3dGeneratedCommandInfo& c = kW3dGeneratedCommands[i];
        if (nombre != c.name) continue;
        r.texto = std::string("type=command\nname=") + c.name +
                  "\nexecute=use tool 'comando' with this command as the first token\n";
        if (*c.example1) r.texto += std::string("example1=") + c.example1 + "\n";
        if (*c.example2) r.texto += std::string("example2=") + c.example2 + "\n";
        if (!*c.example1) r.texto += "examples=(none found in existing .w3s suite)\n";
        return r;
    }

    int mi = -1, ii = -1;
    if (ParseMenuRef(nombre, mi, ii)) {
        PopupMenu* menu = W3dMenuRegistro(mi);
        if (!menu || ii < 0 || ii >= (int)menu->items.size() || !menu->items[(size_t)ii]) {
            r.error = true; r.texto = "menu ref is stale; search the catalog again"; return r;
        }
        MenuItem* it = menu->items[(size_t)ii];
        r.texto = "type=menu\nref=" + nombre + "\npath=" + MenuRuta(menu, it) + "\n";
        const std::string shortcut = MenuAtajoVisible(it);
        if (!shortcut.empty()) r.texto += "shortcut=" + shortcut + "\n";
        if (it->submenu) r.texto += "kind=submenu\n";
        else if (it->checkbox) r.texto += std::string("kind=checkbox\nvalue=") + (*it->checkbox ? "true\n" : "false\n");
        else if (it->valorFloat) {
            char b[128]; snprintf(b, sizeof(b), "kind=slider\\nvalue=%.6g\\nmin=%.6g\\nmax=%.6g\\n", *it->valorFloat, it->vmin, it->vmax);
            r.texto += b;
        } else r.texto += "kind=action\n";
        r.texto += (it->gris && !*it->gris) ? "enabled=false\n" : "enabled=true\n";
        if (!it->submenu) r.texto += "execute=use menu_execute with ref\n";
        return r;
    }

    const int ai = W3dAtajoBuscar(nombre.c_str());
    if (ai >= 0) {
        const W3dAtajoDef& a = W3dAtajoInfo(ai);
        r.texto = std::string("type=shortcut\nid=") + a.id +
                  "\nname=" + a.nombre +
                  "\ncontext=" + W3dAtajoCtxNombre(a.ctx) +
                  "\ndefault=" + a.defecto +
                  "\ncurrent=" + (W3dAtajoTexto(ai).empty() ? "(none)" : W3dAtajoTexto(ai)) +
                  "\nexecute=use tool 'shortcut' with id\n";
        return r;
    }

    for (unsigned i = 0; i < kHerramientasNativasN; i++) {
        if (nombre == kHerramientasNativas[i]) {
            r.texto = "type=tool\nname=" + nombre + "\nexecute=call this tool directly\n";
            return r;
        }
    }

    r.error = true;
    r.texto = "no catalog entry named '" + nombre + "'";
    return r;
}

extern SDL_Keycode W3dTeclaASDLPublica(int);

static SDL_Keymod ModsSDL(int m) {
    int r = KMOD_NONE;
    if (m & W3dMOD_SHIFT) r |= KMOD_SHIFT;
    if (m & W3dMOD_CTRL) r |= KMOD_CTRL;
    if (m & W3dMOD_ALT) r |= KMOD_ALT;
    return (SDL_Keymod)r;
}
static bool EnviarTecla(const W3dCombo& c) {
    const SDL_Keycode k = W3dTeclaASDLPublica(c.tecla);
    if (k == SDLK_UNKNOWN) return false;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = k;
    e.key.keysym.mod = ModsSDL(c.mods);
    e.key.repeat = 0;
    InputUsuarioSDL3(e);
    e.type = SDL_KEYUP;
    InputUsuarioSDL3(e);
    g_redraw = true;
    return true;
}

static Resultado HerrShortcut(const std::string& id) {
    Resultado r;
    const int i = W3dAtajoBuscar(id.c_str());
    if (i < 0) { r.error = true; r.texto = "shortcut desconocido: '" + id + "'"; return r; }

    const W3dAtajoDef& a = W3dAtajoInfo(i);
    int ctxs[4] = {0,0,0,0};
    const int nctx = W3dAtajoContextos(ctxs);
    bool contexto = (a.ctx == W3dATC_GLOBAL);
    for (int k = 0; k < nctx; k++) if (ctxs[k] == a.ctx) contexto = true;
    if (!contexto) {
        r.error = true;
        r.texto = std::string("shortcut '") + a.id + "' requires context " +
                  W3dAtajoCtxNombre(a.ctx) + "; current focus is different";
        return r;
    }

    W3dCombo combo;
    const std::vector<W3dCombo>& actuales = W3dAtajoCombos(i);
    if (!actuales.empty()) combo = actuales[0];
    else {
        std::string d = a.defecto ? a.defecto : "";
        const size_t coma = d.find(',');
        if (coma != std::string::npos) d = d.substr(0, coma);
        if (!W3dComboDesdeTexto(d, combo)) {
            r.error = true;
            r.texto = std::string("shortcut '") + a.id + "' has no invokable key binding; use comando/input";
            return r;
        }
    }

    if (!EnviarTecla(combo)) {
        r.error = true;
        r.texto = "cannot map shortcut key to SDL";
        return r;
    }
    r.texto = std::string("shortcut ejecutado: ") + a.id + " (" + W3dComboTexto(combo) + ")";
    return r;
}

static Resultado HerrInput(const JVal* args) {
    Resultado r;
    const std::string tipo = Texto(Campo(args, "tipo"));
    if (tipo == "key") {
        W3dCombo c;
        const std::string valor = Texto(Campo(args, "valor"));
        if (!W3dComboDesdeTexto(valor, c) || !EnviarTecla(c)) {
            r.error = true; r.texto = "tecla no valida: '" + valor + "'"; return r;
        }
        r.texto = "key: " + valor;
        return r;
    }
    if (tipo == "mouse_move") {
        SDL_Event e; SDL_zero(e); e.type = SDL_MOUSEMOTION;
        e.motion.x = IntArg(Campo(args, "x"), 0);
        e.motion.y = IntArg(Campo(args, "y"), 0);
        e.motion.xrel = IntArg(Campo(args, "dx"), 0);
        e.motion.yrel = IntArg(Campo(args, "dy"), 0);
        InputUsuarioSDL3(e); g_redraw = true;
        r.texto = "mouse moved";
        return r;
    }
    if (tipo == "mouse_button") {
        const std::string b = Texto(Campo(args, "boton"));
        Uint8 boton = b == "right" ? SDL_BUTTON_RIGHT : b == "middle" ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT;
        const bool down = BoolArg(Campo(args, "down"), true);
        SDL_Event e; SDL_zero(e); e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.button = boton;
        e.button.x = IntArg(Campo(args, "x"), 0);
        e.button.y = IntArg(Campo(args, "y"), 0);
        InputUsuarioSDL3(e); g_redraw = true;
        r.texto = std::string("mouse ") + (down ? "down" : "up");
        return r;
    }
    if (tipo == "wheel") {
        SDL_Event e; SDL_zero(e); e.type = SDL_MOUSEWHEEL;
        e.wheel.x = IntArg(Campo(args, "dx"), 0);
        e.wheel.y = IntArg(Campo(args, "dy"), 0);
        InputUsuarioSDL3(e); g_redraw = true;
        r.texto = "wheel";
        return r;
    }
    r.error = true;
    r.texto = "tipo input desconocido (key, mouse_move, mouse_button, wheel)";
    return r;
}

// ---- la lista de herramientas (tools/list) ----
static const char* kHerramientas = R"JSON([
{"name":"guide","description":"Incremental LLM guide. level=0 is the shortest workflow; levels 1..3 add task/session rules, capability discovery and persistence/audit details.","inputSchema":{"type":"object","properties":{"level":{"type":"integer","minimum":0,"maximum":3}}}},
{"name":"state","description":"Compact editor state: project path, unsaved state, interaction mode, active/selected objects, active viewport, and catalog sizes.","inputSchema":{"type":"object","properties":{}}},
{"name":"catalog_index","description":"Coarse function/capability indexes: command alphabet, shortcut contexts, live menu roots and native tool count. Use this before catalog_search on unfamiliar areas.","inputSchema":{"type":"object","properties":{}}},
{"name":"catalog_search","description":"Search every source-derived command, registered editor shortcut, live menu item and native LLM tool. Query terms are ANDed. limit defaults to 40 only to save context; limit=0 returns every match.","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"type":{"type":"string","enum":["all","command","shortcut","menu","tool"]},"limit":{"type":"integer"},"offset":{"type":"integer"}}}},
{"name":"catalog_describe","description":"Describe an exact catalog entry. Commands include real usage examples extracted from the existing .w3s suite; shortcuts include context and bindings.","inputSchema":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}},
{"name":"workspace_status","description":"Persistent agent workspace context. Shows current session/task, sibling tasks, other active sessions, suspended reminders, objective plan and limits. verbose=true also shows closed tasks and registry path.","inputSchema":{"type":"object","properties":{"verbose":{"type":"boolean"}}}},
{"name":"session_open","description":"Open a persistent work session for exactly one objective and make it current. Maximum 3 active sessions.","inputSchema":{"type":"object","properties":{"title":{"type":"string"},"objective":{"type":"string"}},"required":["title","objective"]}},
{"name":"session_plan","description":"Add objective-plan points to the current session. Planning is incremental; tasks must link to one of these point ids.","inputSchema":{"type":"object","properties":{"points":{"type":"array","items":{"type":"string"},"minItems":1}},"required":["points"]}},
{"name":"session_switch","description":"Switch to another active session. Its active task, if any, becomes current.","inputSchema":{"type":"object","properties":{"session_id":{"type":"string"}},"required":["session_id"]}},
{"name":"session_close","description":"Close the current session. Objective result is derived automatically: confirmed when every plan point is complete, missing when every point is missing, otherwise partial. Optional result can assert the expected derived value. Requires all tasks closed, all plan points resolved and at least one session_test; confirmed also requires the latest test to pass.","inputSchema":{"type":"object","properties":{"result":{"type":"string","enum":["confirmed","partial","missing"]},"comment":{"type":"string"}},"required":["comment"]}},
{"name":"task_open","description":"Open a task linked to one objective plan point and make it current. Maximum 8 active/suspended tasks per session.","inputSchema":{"type":"object","properties":{"title":{"type":"string"},"point_id":{"type":"string"}},"required":["title","point_id"]}},
{"name":"task_switch","description":"Switch to another active task in the current session.","inputSchema":{"type":"object","properties":{"task_id":{"type":"string"}},"required":["task_id"]}},
{"name":"task_suspend","description":"Suspend the current task without losing its state; suspended tasks remain visible in every agent context.","inputSchema":{"type":"object","properties":{"comment":{"type":"string"}},"required":["comment"]}},
{"name":"task_resume","description":"Resume a suspended task in the current session and make it current.","inputSchema":{"type":"object","properties":{"task_id":{"type":"string"}},"required":["task_id"]}},
{"name":"task_commit","description":"Create a persistent internal task checkpoint with timestamp, current automatic change-index position, summary and mandatory internal comment. This is entirely inside the LLM workspace protocol.","inputSchema":{"type":"object","properties":{"summary":{"type":"string"},"comment":{"type":"string"}},"required":["summary","comment"]}},
{"name":"task_close","description":"Close the current task. Requires at least one internal task_commit and resolves its linked plan point as complete, partial or missing.","inputSchema":{"type":"object","properties":{"outcome":{"type":"string","enum":["complete","partial","missing"]},"comment":{"type":"string"}},"required":["outcome","comment"]}},
{"name":"session_test","description":"Run real editor commands as a session test and persist actual output. The test passes only when command execution succeeds and optional expect_contains is present.","inputSchema":{"type":"object","properties":{"commands":{"type":"string"},"expect_contains":{"type":"string"}},"required":["commands"]}},
{"name":"menu_execute","description":"Execute a terminal live menu item returned by catalog_search type=menu. Checkboxes toggle; sliders require value. Runtime refs can become stale after UI rebuilds, so search again if needed.","inputSchema":{"type":"object","properties":{"ref":{"type":"string"},"value":{"type":"number"}},"required":["ref"]}},
{"name":"shortcut","description":"Invoke any registered editor shortcut by stable shortcut id using the editor's real input path and current context.","inputSchema":{"type":"object","properties":{"id":{"type":"string"}},"required":["id"]}},
{"name":"input","description":"Inject generic editor input through the real SDL input path. Types: key (valor like Ctrl+S), mouse_move, mouse_button, wheel. This is the unrestricted UI fallback when no semantic command exists.","inputSchema":{"type":"object","properties":{"tipo":{"type":"string","enum":["key","mouse_move","mouse_button","wheel"]},"valor":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"dx":{"type":"integer"},"dy":{"type":"integer"},"boton":{"type":"string","enum":["left","middle","right"]},"down":{"type":"boolean"}},"required":["tipo"]}},
{"name":"escena","description":"El arbol de objetos de la escena de Whisk3D: nombre, tipo, posicion, rotacion (euler en grados), escala, y cual esta seleccionado y cual es el activo. Las coordenadas son las del panel Propiedades: Z para arriba.","inputSchema":{"type":"object","properties":{}}},
{"name":"captura","description":"Una imagen PNG de lo que muestra el editor, para ver el resultado. Por defecto solo la vista 3D activa; con zona='ventana', el editor entero. Se achica hasta ancho_max pixeles (800 si no se dice).","inputSchema":{"type":"object","properties":{"zona":{"type":"string","enum":["vista3d","ventana"]},"ancho_max":{"type":"integer"}}}},
{"name":"capture_file","description":"Guarda el framebuffer actual en un PNG exacto y devuelve solo path, dimensiones y bytes, sin base64. zona=3d captura solo el viewport 3D; zona=ventana captura la ventana completa.","inputSchema":{"type":"object","properties":{"path":{"type":"string"},"zona":{"type":"string","enum":["3d","ventana"]},"ancho_max":{"type":"integer","minimum":64}},"required":["path"]}},
{"name":"object_create","description":"Crea una primitiva, la nombra y aplica posicion/rotacion/escala en una sola llamada. Devuelve el estado final del objeto; usa coordenadas del panel Propiedades (Z arriba).","inputSchema":{"type":"object","properties":{"tipo":{"type":"string","enum":["cube","plane","circle","uvsphere","cone","cylinder","vertex","empty","camera","light"]},"nombre":{"type":"string"},"posicion":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"rotacion":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"escala":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},"required":["tipo","nombre"]}},
{"name":"parent_keep","description":"Reparenta un objeto conservando exactamente su transformacion global. Util para construir rigs/jerarquias sin mover las piezas.","inputSchema":{"type":"object","properties":{"child":{"type":"string"},"parent":{"type":"string"}},"required":["child","parent"]}},
{"name":"agregar_objeto","description":"Agrega un objeto en el cursor 3D, como el menu Add. Queda seleccionado y activo.","inputSchema":{"type":"object","properties":{"tipo":{"type":"string","enum":["cube","plane","circle","uvsphere","cone","cylinder","vertex","empty","camera","light"]}},"required":["tipo"]}},
{"name":"seleccionar","description":"Selecciona un objeto por su nombre y lo deja activo (las herramientas que no reciben nombre actuan sobre el activo). Con agregar=true suma a la seleccion; sin nombre, deselecciona todo.","inputSchema":{"type":"object","properties":{"nombre":{"type":"string"},"agregar":{"type":"boolean"}}}},
{"name":"transformar","description":"Cambia la posicion, la rotacion (euler en grados) o la escala de un objeto (por nombre, o el activo), con las coordenadas del panel Propiedades (Z para arriba). Solo cambia lo que se pasa. Se deshace con Ctrl+Z.","inputSchema":{"type":"object","properties":{"nombre":{"type":"string"},"posicion":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"rotacion":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"escala":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}}}},
{"name":"borrar_seleccion","description":"Borra los objetos seleccionados (se deshace con Ctrl+Z).","inputSchema":{"type":"object","properties":{}}},
{"name":"deshacer","description":"Deshace la ultima accion (Ctrl+Z), o la rehace con rehacer=true.","inputSchema":{"type":"object","properties":{"rehacer":{"type":"boolean"}}}},
{"name":"abrir_proyecto","description":"Abre un proyecto .w3d (cierra el actual).","inputSchema":{"type":"object","properties":{"ruta":{"type":"string"}},"required":["ruta"]}},
{"name":"guardar_proyecto","description":"Guarda el proyecto en un .w3d.","inputSchema":{"type":"object","properties":{"ruta":{"type":"string"}},"required":["ruta"]}},
{"name":"importar_modelo","description":"Importa un modelo .obj, .fbx, .gltf o .glb, como el menu Add > Import.","inputSchema":{"type":"object","properties":{"ruta":{"type":"string"}},"required":["ruta"]}},
{"name":"command","description":"Universal semantic execution surface. Same engine as the legacy 'comando' tool; accepts one or more source-derived editor commands, one per line.","inputSchema":{"type":"object","properties":{"commands":{"type":"string"}},"required":["commands"]}},
{"name":"comando","description":"Universal semantic execution surface: one or more editor commands, one per line. The build-generated catalog currently exposes every command found in all main/test dispatchers; use catalogo_buscar/catalogo_descrivere to discover syntax and real examples. Stops at the first failing command and returns its output. Algunos: mode <object|edit> | select <all|none> | selmode <vertex|edge|face> | extrude <dist> | loopcut <arista> <cortes> <factor> | merge <center|bydistance> | triangulate | separate | duplicate | join | shade <smooth|flat> | apply <location|rotation|scale|all> | objname <nombre> | parent <hijo> <padre> | modadd <screw|mirror|array|subsurf|boolean> | modapply | matnew | matcolor <r> <g> <b> (0 a 1) | newtex <nombre> <ancho> <alto> <blank|uvgrid|colorgrid> | unwrap <angle|conformal> | smartuv | addarmature | timeline <play|pause|goto N> | frame N | export <ruta.obj> | exportgltf <ruta> | exportglb <ruta> | rtrender <ancho> <alto> | xray <0|1> | undo | redo.","inputSchema":{"type":"object","properties":{"comandos":{"type":"string"}},"required":["comandos"]}}
])JSON";

static Resultado Llamar(const std::string& nombre, const JVal* args) {
    if (nombre == "guide" || nombre == "guia") return HerrGuia(args);
    if (nombre == "state" || nombre == "stato") return HerrStato();
    if (nombre == "catalog_index") return HerrCatalogIndex();
    if (nombre == "catalog_search" || nombre == "catalogo_buscar") return HerrCatalogoBuscar(args);
    if (nombre == "catalog_describe" || nombre == "catalogo_descrivere") {
        std::string n = Texto(Campo(args, "name"));
        if (n.empty()) n = Texto(Campo(args, "nombre"));
        return HerrCatalogoDescrivere(n);
    }
    if (nombre == "workspace_status") {
        return DesdeWorkspace(W3dLlmWorkspaceResult(
            true, W3dLlmWorkspaceContext(BoolArg(Campo(args, "verbose"), false))));
    }
    if (nombre == "session_open")
        return DesdeWorkspace(W3dLlmSessionOpen(
            Texto(Campo(args, "title")), Texto(Campo(args, "objective"))));
    if (nombre == "session_plan")
        return DesdeWorkspace(W3dLlmSessionAddPlan(ListaTexto(Campo(args, "points"))));
    if (nombre == "session_switch")
        return DesdeWorkspace(W3dLlmSessionSwitch(Texto(Campo(args, "session_id"))));
    if (nombre == "session_close")
        return DesdeWorkspace(W3dLlmSessionClose(
            Texto(Campo(args, "result")), Texto(Campo(args, "comment"))));
    if (nombre == "task_open")
        return DesdeWorkspace(W3dLlmTaskOpen(
            Texto(Campo(args, "title")), Texto(Campo(args, "point_id"))));
    if (nombre == "task_switch")
        return DesdeWorkspace(W3dLlmTaskSwitch(Texto(Campo(args, "task_id"))));
    if (nombre == "task_suspend")
        return DesdeWorkspace(W3dLlmTaskSuspend(Texto(Campo(args, "comment"))));
    if (nombre == "task_resume")
        return DesdeWorkspace(W3dLlmTaskResume(Texto(Campo(args, "task_id"))));
    if (nombre == "task_commit")
        return DesdeWorkspace(W3dLlmTaskCommit(
            Texto(Campo(args, "summary")), Texto(Campo(args, "comment"))));
    if (nombre == "task_close")
        return DesdeWorkspace(W3dLlmTaskClose(
            Texto(Campo(args, "outcome")), Texto(Campo(args, "comment"))));
    if (nombre == "session_test") return HerrSessionTest(args);
    if (nombre == "menu_execute") return HerrMenuExecute(args);
    if (nombre == "shortcut") return HerrShortcut(Texto(Campo(args, "id")));
    if (nombre == "input") return HerrInput(args);
    if (nombre == "escena") return HerrEscena();
    if (nombre == "capture_file") return HerrCaptureFile(args);
    if (nombre == "captura") {
        const JVal* am = Campo(args, "ancho_max");
        return HerrCaptura(Texto(Campo(args, "zona")) != "ventana", (am && am->tipo == 1) ? (int)am->num : 800);
    }
    if (nombre == "object_create") return HerrObjectCreate(args);
    if (nombre == "parent_keep") return HerrParentKeep(args);
    if (nombre == "agregar_objeto") return HerrAgregar(Texto(Campo(args, "tipo")));
    if (nombre == "seleccionar") {
        const JVal* ag = Campo(args, "agregar");
        return HerrSeleccionar(Texto(Campo(args, "nombre")), ag && ag->tipo == 3 && ag->b);
    }
    if (nombre == "transformar") return HerrTransformar(args);
    if (nombre == "borrar_seleccion") return HerrComandos("delete");
    if (nombre == "deshacer") {
        const JVal* re = Campo(args, "rehacer");
        return HerrComandos((re && re->tipo == 3 && re->b) ? "redo" : "undo");
    }
    if (nombre == "abrir_proyecto") return HerrComandos("abrirw3d " + Texto(Campo(args, "ruta")));
    if (nombre == "guardar_proyecto") return HerrComandos("guardarw3d " + Texto(Campo(args, "ruta")));
    if (nombre == "importar_modelo") return HerrImportar(Texto(Campo(args, "ruta")));
    if (nombre == "command") return HerrComandos(Texto(Campo(args, "commands")));
    if (nombre == "comando") return HerrComandos(Texto(Campo(args, "comandos")));
    Resultado r;
    r.error = true;
    r.texto = "herramienta desconocida: '" + nombre + "'";
    return r;
}

static bool DebeRecordarContexto(const std::string& toolName, bool error) {
    if (error) { gToolCallsDesdeContexto = 0; return true; }
    if (toolName == "workspace_status" || toolName == "guide" || toolName == "guia" ||
        toolName.compare(0, 8, "session_") == 0 ||
        toolName.compare(0, 5, "task_") == 0) {
        gToolCallsDesdeContexto = 0;
        return true;
    }
    gToolCallsDesdeContexto++;
    if (gToolCallsDesdeContexto >= 8) {
        gToolCallsDesdeContexto = 0;
        return true;
    }
    return false;
}

// ============================================================================
//  El protocolo
// ============================================================================
static void Atender(const std::string& linea) {
    LectorJson lj(linea);
    JVal* msj = lj.Valor();
    if (lj.error || msj->tipo != 4) {
        Enviar("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"JSON invalido\"}}");
        delete msj;
        return;
    }
    const std::string metodo = Texto(Campo(msj, "method"));
    const JVal* idv = Campo(msj, "id");
    const std::string id = IdTexto(idv);
    const JVal* params = Campo(msj, "params");
    if (!idv) { delete msj; return; }   // una notificacion (notifications/initialized, cancelled...): sin respuesta
    std::string res;
    if (metodo == "initialize") {
        // la version que pide el cliente, si es una de estas; si no, la ultima
        std::string ver = Texto(Campo(params, "protocolVersion"));
        if (ver != "2024-11-05" && ver != "2025-03-26" && ver != "2025-06-18") ver = "2025-06-18";
        std::string instructions =
            "You control the live Whisk3D editor through a persistent task/session protocol. "
            "Start with guide(level=0) and workspace_status. Before any mutation: open or switch "
            "a session, create its objective plan, then open/resume a task linked to one plan point. "
            "Use catalog_index/catalog_search/catalog_describe instead of guessing capabilities. "
            "command exposes every source-derived command; shortcut, menu_execute and input expose "
            "native UI paths. Successful mutations are automatically indexed in the current task. "
            "Checkpoint with task_commit(summary, comment), close tasks with complete/partial/missing, "
            "run session_test, then let session_close derive confirmed/partial/missing from the plan. "
            "Context reminders include current session/task and suspended work periodically, and always after errors or session/task operations.";
        if (W3dLlmWorkspaceReady())
            instructions += "\n" + W3dLlmWorkspaceContext(false);
        res = "{\"protocolVersion\":" + Esc(ver) + ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
              "\"serverInfo\":{\"name\":\"whisk3d\",\"version\":" + Esc(W3dVersion()) + "},"
              "\"instructions\":" + Esc(instructions) + "}";
    } else if (metodo == "ping") {
        res = "{}";
    } else if (metodo == "tools/list") {
        // (la lista esta escrita en varias lineas para leerla aca; el protocolo es un mensaje por linea)
        std::string lista;
        for (const char* c = kHerramientas; *c; c++) if (*c != '\n' && *c != '\r') lista += *c;
        res = "{\"tools\":" + lista + "}";
    } else if (metodo == "tools/call") {
        const std::string toolName = Texto(Campo(params, "name"));
        const JVal* toolArgs = Campo(params, "arguments");
        Resultado r;
        if (HerramientaMuta(toolName)) {
            std::string reason;
            if (!W3dLlmWorkspaceCanMutate(reason)) {
                r.error = true;
                r.texto = "mutation blocked by task protocol: " + reason;
            } else {
                r = Llamar(toolName, toolArgs);
                if (!r.error)
                    W3dLlmWorkspaceRecordChange(toolName, DetalleCambio(toolName, toolArgs, r));
            }
        } else {
            r = Llamar(toolName, toolArgs);
        }

        res = "{\"content\":[";
        if (!r.imagenPng.empty())
            res += "{\"type\":\"image\",\"data\":\"" + Base64(r.imagenPng) + "\",\"mimeType\":\"image/png\"},";
        res += "{\"type\":\"text\",\"text\":" + Esc(r.texto) + "}";
        if (W3dLlmWorkspaceReady() && DebeRecordarContexto(toolName, r.error)) {
            res += ",{\"type\":\"text\",\"text\":" +
                   Esc(W3dLlmWorkspaceContext(false)) + "}";
        }
        res += "],\"isError\":" + std::string(r.error ? "true" : "false") + "}";
    } else {
        Enviar("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":-32601,\"message\":" + Esc("metodo desconocido: " + metodo) + "}}");
        delete msj;
        return;
    }
    Enviar("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + res + "}");
    delete msj;
}

bool W3dLlmAtender() {
    if (!gActivo) return true;
    if (SDL_AtomicGet(&gTipoEvento) == 0) {
        const Uint32 t = SDL_RegisterEvents(1);
        SDL_AtomicSet(&gTipoEvento, t == (Uint32)-1 ? -1 : (int)t);
    }
    for (;;) {
        std::string p;
        SDL_LockMutex(gCola);
        if (!gPedidos.empty()) { p = gPedidos.front(); gPedidos.pop_front(); }
        SDL_UnlockMutex(gCola);
        if (p.empty()) break;
        Atender(p);
    }
    return SDL_AtomicGet(&gCerrado) == 0;
}

#endif // W3D_MCP
