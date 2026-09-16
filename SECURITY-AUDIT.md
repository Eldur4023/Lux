# Auditoría de seguridad — Lux

Revisión manual del framework (núcleo C++20 + LuxScript) centrada en las superficies
expuestas a entrada no confiable: parser HTTP, capa de conexión, ficheros estáticos,
plantillas, sesiones/JWT, capa SQL, VM, y los módulos nativos (`regex`, `http`, `os`,
`csv`, multipart, WebSocket).

**Commit auditado:** `d10f274`
**Fecha:** 2026-09-16
**Fecha de remediación:** 2026-09-16 (rama `security-fixes`)

Los hallazgos marcados **[verificado]** se reprodujeron con un programa de prueba
compilado aparte; el resto son resultado de lectura de código.

Los hallazgos 1-8, 11-13 y 17 quedaron corregidos y verificados (recompilación limpia,
suite de tests existente sin regresiones nuevas, y prueba manual contra el binario real
para los tres primeros). El detalle de cada corrección está al final del documento, en
«Estado de la remediación». Los hallazgos 9, 10, 14, 15 y 16 son decisiones de diseño o
producto que requieren una elección del mantenedor, no un bug con una corrección
mecánica; se explica en cada uno por qué no se tocaron.

---

## Resumen

| # | Hallazgo | Severidad | Estado |
|---|---|---|---|
| 1 | `regex.*` desborda la pila y mata el proceso entero con ~30 KB de entrada | **Crítica** | ✅ Corregido |
| 2 | Divergencia `--native` vs bytecode en la coerción de params `int` | **Alta** | ✅ Corregido |
| 3 | `std::mismatch` sobre `fs::path` lee fuera de rango → SEGV en ficheros estáticos | **Alta** | ✅ Corregido |
| 4 | `verify_jwt` no rechaza un token sin `iss` cuando hay issuer configurado | Media | ✅ Corregido |
| 5 | Sin límite de tamaño en respuestas del cliente `http.*` (OOM) | Media | ✅ Corregido |
| 6 | Cabeceras salientes de `http.*` sin saneado CRLF | Media | ✅ Corregido |
| 7 | `read_buf` de WebSocket crece sin límite tras un fallo de parseo | Media | ✅ Corregido |
| 8 | Params `float` aceptan `nan`, `inf`, `0x10` | Media | ✅ Corregido |
| 9 | SSRF: `http.*` sin allowlist ni bloqueo de redes internas | Media | ⚠️ Parcial — ver nota |
| 10 | Los montajes estáticos saltan toda la cadena de middleware | Media | ⏭ No tocado — diseño |
| 11 | `Content-Length` duplicable por el handler | Baja | ✅ Corregido |
| 12 | Inyección de fórmulas en `csv.*` | Baja | ✅ Corregido |
| 13 | `random_bytes` devuelve vacío en silencio si falla | Baja | ✅ Corregido |
| 14 | Sin rate limiting ni protección de fuerza bruta | Baja (diseño) | ⏭ No tocado — feature nueva |
| 15 | `/metrics` y health sin autenticación por defecto | Informativa | ⏭ No tocado — decisión de despliegue |
| 16 | La VM no comprueba límites de pila ni de locals | Informativa | ⏭ No tocado — no explotable por red |
| 17 | Comentario falso sobre `fd_ = -1` tras `close()` | Informativa | ✅ Corregido |

---

## 1. `regex.*` desborda la pila y mata el proceso entero — **Crítica** [verificado]

**Dónde:** [module_regex.cpp:34](src/lux_script/module_regex.cpp#L34) y las seis
funciones registradas en [module_regex.cpp:124-129](src/lux_script/module_regex.cpp#L124-L129).

`std::regex` de libstdc++ ejecuta la búsqueda de forma recursiva, un nivel de pila por
carácter del sujeto. No es un problema de patrones patológicos: se reproduce con el
patrón más trivial posible.

Medido en esta máquina (pila de 8 MB, `ulimit -s 8192`), con `std::regex_search`:

| Patrón | Long. sujeto | Resultado |
|---|---|---|
| `[a-z]+` | 25 000 | ok |
| `[a-z]+` | 30 000 | **SIGSEGV** |
| `^[\w.+-]+@[\w-]+\.[\w.]+$` (validación de email de manual) | 50 000 | **SIGSEGV** |
| `(\s*\w+)*` | 50 000 | **SIGSEGV** |
| `^(a\|aa)+$` | 1 000 | cuelgue >15 s (backtracking) |

El umbral está en ~30 KB. El límite de cuerpo del framework es 16 MB
([http_parser.hpp:30](src/http/http_parser.hpp#L30)), así que un solo `POST` con un
campo de 30 KB lo alcanza sobradamente.

Dos consecuencias, y la segunda es la grave:

- **Cuelgue:** ninguna función de `regex` está marcada `is_async`, así que todas corren
  en el hilo del event loop. Un backtracking catastrófico no bloquea una petición: bloquea
  el núcleo entero y todas las conexiones que ese hilo estaba sirviendo.
- **Caída total:** Lux es *un proceso* con N hilos repartiéndose conexiones vía
  `SO_REUSEPORT`, y sólo instala manejadores para `SIGINT`/`SIGTERM`/`SIGPIPE`
  ([app.cpp:369,485-486](src/app.cpp#L369)). Un SIGSEGV en cualquier hilo tumba el
  proceso completo: todos los núcleos, todas las conexiones, sin reinicio.

Es decir: **cualquier app que pase datos de la petición a `regex.*` —que es exactamente
para lo que existe el módulo— tiene un DoS remoto, no autenticado, de una sola petición,
que provoca una caída total del servicio.**

El comentario de cabecera del módulo documenta el coste de recompilar el patrón en cada
llamada como la limitación conocida; el problema real es otro y no está mencionado.

---

## 2. Divergencia `--native` vs bytecode en la coerción de params `int` — **Alta** [verificado]

**Dónde:** [project.cpp:641-651](src/lux_script/project.cpp#L641-L651) (bytecode) contra
[native_gen.cpp:2890-2900](src/lux_script/native_gen.cpp#L2890-L2900) (nativo).

El intérprete exige que `std::stoll` consuma el texto entero:

```cpp
size_t pos = 0;
long long v = std::stoll(text, &pos);
if (pos != text.size()) return false;
```

El código generado para `--native` no:

```cpp
inline bool lux_route_coerce_int(const std::string& t, int64_t& out) {
    try { out = std::stoll(t); return true; } catch (...) { return false; }
}
```

El comentario que precede a esa función afirma que la omisión es deliberada, «replicar el
mismo comportamiento del intérprete bit a bit, para que `--native` nunca acepte (o rechace)
un valor que bytecode habría tratado distinto». Esa afirmación ya no es cierta: el
intérprete se endureció y el backend nativo no se actualizó. Ambos sitios entraron en el
árbol en el mismo commit (`052f7cd`, el rename Lumen→Lux), así que la divergencia viene de
antes de esa reescritura.

Comportamiento medido para `get endpoint("/users/:id", int id)`:

| URL | bytecode | `--native` |
|---|---|---|
| `/users/12abc` | 400 | **`id = 12`** |
| `/users/1e999` | 400 | **`id = 1`** |
| `/users/0x1p4` | 400 | **`id = 0`** |
| `/users/0x10` | 400 | **`id = 0`** |

Impacto:

- La misma app cambia de comportamiento al compilar con `--native`, que es justo lo que el
  comentario promete que no puede pasar. Una suite de tests en modo bytecode no detecta nada.
- **Parser differential:** cualquier cosa que actúe sobre la ruta cruda —un middleware de
  autorización, una clave de caché, un WAF por delante, los logs de auditoría— ve `12abc`
  mientras el handler opera sobre el registro `12`. `/users/12abc`, `/users/12%20`,
  `/users/12x` y `/users/12` son todas el mismo recurso para el handler y cuatro cadenas
  distintas para todo lo demás.

---

## 3. `std::mismatch` sobre `fs::path` lee fuera de rango — **Alta** [verificado]

**Dónde:** [app.cpp:140-141](src/app.cpp#L140-L141), y de nuevo en
[app.cpp:164-166](src/app.cpp#L164-L166) y [app.cpp:184-185](src/app.cpp#L184-L185).

```cpp
auto [ri, fi] = std::mismatch(canonical_root.begin(), canonical_root.end(),
                              preliminary.begin());
```

La sobrecarga de tres iteradores recorre el rango completo de `canonical_root` sin saber
dónde acaba `preliminary`. Si la ruta resuelta tiene **menos componentes** que la raíz,
`mismatch` desreferencia el iterador `end()` de `preliminary`.

Comprobado con `-fsanitize=address` sobre root `/srv/www/public` y resuelto `/srv`:

```
AddressSanitizer: SEGV on unknown address 0x000000000028
  #0 std::filesystem::path::compare(...) const
  #5 std::mismatch<path::iterator, path::iterator>(...)
```

No es UB teórico: es un SEGV duro, y por el mismo motivo del hallazgo 1 se lleva el proceso
entero por delante.

El guardia de dotfiles de [app.cpp:114-120](src/app.cpp#L114-L120) rechaza cualquier `/.`
antes de llegar aquí, así que `..` no es la vía. La vía es un **symlink dentro de la raíz
servida que apunte a un directorio menos profundo** — `public/assets -> /opt/assets` con
la raíz en `/home/user/app/public`, por ejemplo. `weakly_canonical` lo resuelve a algo más
corto que la raíz y la comprobación revienta.

Lo irónico es que esta es precisamente la rama que existe para *devolver 403* ante un
symlink que se escapa de la raíz: en el caso que más importa, en vez de bloquear, cae.

---

## 4. `verify_jwt` acepta tokens sin `iss` — Media

**Dónde:** [auth.cpp:100-105](src/lux_script/auth.cpp#L100-L105).

```cpp
if (!issuer.empty()) {
    auto it = payload.as_dict().find("iss");
    if (it != payload.as_dict().end() &&
        (!it->second.is_str() || it->second.as_str() != issuer))
        return false;
}
```

La validación sólo se aplica **si el claim está presente**. Un token sin `iss` pasa la
comprobación aunque haya un issuer configurado. Lo correcto es que la ausencia sea un
rechazo cuando el issuer es obligatorio.

Explotable cuando el mismo secreto HMAC se comparte entre servicios (patrón habitual): un
token emitido por otro servicio del mismo dominio de confianza, o uno acuñado sin `iss`,
es aceptado por esta ruta.

En la misma función, `exp` sólo se comprueba si existe y es numérico
([auth.cpp:95-99](src/lux_script/auth.cpp#L95-L99)) — un token sin `exp` no caduca nunca —
y `nbf` no se comprueba en absoluto.

Nota aparte, de diseño: la cookie de sesión va **firmada pero no cifrada**
([auth.cpp:14-22](src/lux_script/auth.cpp#L14-L22) — base64url de JSON en claro). Es el
modelo de Flask y es defendible, pero conviene que la documentación lo diga explícitamente
para que nadie meta datos sensibles en `session`.

---

## 5. Sin límite de tamaño en respuestas de `http.*` — Media

**Dónde:** [module_http.cpp:52-55](src/lux_script/module_http.cpp#L52-L55).

```cpp
size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}
```

Sin `CURLOPT_MAXFILESIZE` y sin corte en el callback. Un servidor remoto malicioso o
comprometido devuelve un cuerpo ilimitado y agota la RAM del proceso. `write_header`
([module_http.cpp:59](src/lux_script/module_http.cpp#L59)) tiene el mismo problema sobre el
`Dict` de cabeceras.

El timeout de 15 s acota el tiempo pero no los bytes: a un ancho de banda normal caben
varios GB en esa ventana.

---

## 6. Cabeceras salientes de `http.*` sin saneado CRLF — Media

**Dónde:** [module_http.cpp:100-102](src/lux_script/module_http.cpp#L100-L102).

```cpp
std::string line = key + ": " + val.as_str();
header_list = curl_slist_append(header_list, line.c_str());
```

Ni la clave ni el valor se filtran. Un `\r\n` en cualquiera de los dos inyecta cabeceras
arbitrarias en la petición saliente (request splitting contra el servicio de destino), y un
NUL embebido trunca la cabecera por el `c_str()`.

Es asimétrico respecto al resto del framework: `Response::header`
([response.hpp:82-91](include/lux/response.hpp#L82-L91)) y `build_set_cookie`
([cookies.hpp:44-63](include/lux/cookies.hpp#L44-L63)) sí filtran CR/LF con cuidado. La
ruta saliente se quedó sin ese tratamiento.

---

## 7. `read_buf` de WebSocket crece sin límite tras un fallo de parseo — Media

**Dónde:** [websocket.hpp:314](include/lux/websocket.hpp#L314) junto con
[websocket.hpp:235](include/lux/websocket.hpp#L235) y
[websocket.hpp:243](include/lux/websocket.hpp#L243).

```cpp
void feed(const uint8_t* data, size_t len) {
    read_buf.append(reinterpret_cast<const char*>(data), len);
    try_parse();
    resume_waiter_if_ready();
}
```

`feed()` no mira `closed` antes de acumular. Dos rutas de `try_parse()` hacen
`closed = true; return;` sin enviar frame de cierre y sin que nadie cierre el socket:

- frame mayor que `kMaxFramePayload` ([websocket.hpp:235](include/lux/websocket.hpp#L235))
- `pending.size() >= kMaxPendingFrames` ([websocket.hpp:243](include/lux/websocket.hpp#L243))

`closed` es sólo una bandera. La conexión TCP sigue viva hasta que el *handler* termine su
corrutina, y `_ws_on_data` sólo se desengancha en `HttpConnection::close()`
([http_connection.cpp:637-640](src/http/http_connection.cpp#L637-L640)). Si el handler está
suspendido en un `await` largo —una consulta lenta, un `sleep`—, el cliente puede seguir
bombeando bytes indefinidamente: cada uno se anexa a `read_buf`, `try_parse()` sale en la
primera comprobación y nada libera memoria.

El camino normal está acotado en ~16 MB por conexión; estas dos ramas no tienen cota ninguna.

Menor, en la misma zona: la longitud no exige codificación mínima (se acepta `len7 == 126`
con un payload de 3 bytes), lo que permite desacuerdos con intermediarios.

---

## 8. Params `float` aceptan `nan`, `inf` y hexadecimal — Media [verificado]

**Dónde:** [project.cpp:653-658](src/lux_script/project.cpp#L653-L658).

`std::stod` consume `"nan"`, `"inf"` y `"-inf"` por completo, así que la comprobación
`pos != text.size()` los da por válidos. También acepta flotantes hexadecimales
(`"0x10"` → 16, `"0x1p4"` → 16) y espacios a la izquierda (`"  42"`, alcanzable vía `%20`).
Afecta a **los dos backends**.

Un `float precio` que reciba `nan` hace que toda comparación de orden sea falsa: un guardia
del estilo `require precio > 0 else ...` no dispara, y `precio != precio` es cierto. Es un
bypass de validación silencioso.

La serialización de salida sí está protegida: `write_double`
([value.cpp:247-256](src/lux_script/value.cpp#L247-L256)) convierte los no finitos a `null`,
así que el JSON de respuesta no se corrompe. El problema es la lógica, no el formato.

---

## 9. SSRF: `http.*` sin allowlist ni bloqueo de redes internas — Media

**Dónde:** [module_http.cpp:85-88](src/lux_script/module_http.cpp#L85-L88) y
[module_http.cpp:134-135](src/lux_script/module_http.cpp#L134-L135).

La única validación de la URL es que empiece por `http://` o `https://`. No hay bloqueo de
loopback, RFC1918, ni link-local — `http.get("http://169.254.169.254/...")` (metadatos de
nube) o `http://127.0.0.1:5000/admin` funcionan.

Además `CURLOPT_FOLLOWLOCATION` está activo con 5 saltos y **sin `CURLOPT_REDIR_PROTOCOLS`
explícito**. Aunque el destino inicial se valide a nivel de aplicación, un redirect 302
lleva la petición a donde quiera el remoto. Conviene fijar los protocolos permitidos de
forma explícita en vez de depender del default de la versión de libcurl enlazada.

No hay forma de desactivar el seguimiento de redirects desde LuxScript.

---

## 10. Los montajes estáticos saltan toda la cadena de middleware — Media

**Dónde:** [app.cpp:286-290](src/app.cpp#L286-L290).

```cpp
// Static file mounts bypass the middleware chain.
if (req.method == "GET" || req.method == "HEAD") {
    if (try_serve_static(static_mounts_, req, res)) co_return;
}
```

Es intencional y está comentado, pero la consecuencia merece quedar escrita: un
`group("/admin"): require session.role == "admin"` **no protege** un `static` montado bajo
ese prefijo. El guardia ni se ejecuta.

Relacionado: el `path` que ven los middlewares es el **crudo**
([http_connection.cpp:213](src/http/http_connection.cpp#L213) lo asigna sin decodificar),
mientras que el servidor de ficheros decodifica con `url_decode_path`
([app.cpp:72-93](src/app.cpp#L72-L93)). Para rutas normales, un middleware que compare
`req.path` contra un prefijo ve una cadena distinta de la que se resuelve contra el disco.

Nota de diseño adyacente: `File.save()` ([natives.cpp:909-974](src/lux_script/natives.cpp#L909-L974))
no restringe extensiones, y `safe_name` ([natives.cpp:539-544](src/lux_script/natives.cpp#L539-L544))
sólo quita separadores de directorio y rechaza `.`/`..`. Subir `evil.html` o `evil.svg` a un
directorio que además esté montado como `static` da XSS almacenado servido con su
`Content-Type` real. El guardia de dotfiles cubre `.env`, pero no esto.

---

## 11. `Content-Length` duplicable por el handler — Baja

**Dónde:** [response.hpp:292-310](include/lux/response.hpp#L292-L310).

`build()` emite `Content-Length` incondicionalmente y *después* `emit_headers()` vuelca el
mapa de cabeceras tal cual. Un handler que haga `res.header("Content-Length", ...)` o
`res.header("Transfer-Encoding", "chunked")` produce una respuesta con cabeceras
duplicadas/contradictorias — la base de un request smuggling si hay un proxy delante que
resuelva el conflicto de forma distinta a como lo hace el cliente.

El filtrado CR/LF de `header()` evita el response splitting clásico, pero no impide
sobrescribir cabeceras de framing. Debería haber una lista de cabeceras reservadas que el
handler no pueda emitir.

Menor: las respuestas 204 y 304 también llevan `Content-Length`.

---

## 12. Inyección de fórmulas en `csv.*` — Baja

**Dónde:** [module_csv.cpp:107-108](src/lux_script/module_csv.cpp#L107-L108).

```cpp
bool needs_quotes = s.find_first_of(",\"\n\r") != std::string::npos;
```

El escapado es correcto para RFC 4180, pero un campo que empiece por `=`, `+`, `-`, `@`,
TAB o CR se interpreta como fórmula al abrir el fichero en Excel, LibreOffice o Sheets.
Exportar datos de usuario a CSV es el caso de uso del módulo, así que la mitigación
(prefijar un `'`) encaja aquí.

---

## 13. `random_bytes` falla en silencio — Baja

**Dónde:** [crypto.cpp:223-232](src/lux_script/crypto.cpp#L223-L232).

```cpp
std::FILE* f = std::fopen("/dev/urandom", "rb");
if (!f) return {};
...
if (got != n) return {};
```

Devuelve una cadena **vacía** en caso de fallo, indistinguible de un resultado válido para
el llamante. Si `/dev/urandom` no abre —agotamiento de descriptores bajo carga, un chroot
mal montado— los consumidores reciben cero bytes de entropía sin enterarse.

Consumidores actuales: `hash.random_bytes` ([module_hash.cpp:47](src/lux_script/module_hash.cpp#L47)),
expuesto a LuxScript y candidato natural a generar tokens; y el sufijo anticolisión de
`File.save()` ([natives.cpp:947-949](src/lux_script/natives.cpp#L947-L949)), donde un
resultado vacío hace que los 5 reintentos generen el mismo nombre.

Debería ser un fallo duro, y conviene migrar a `getrandom(2)`, que no depende de tener un
descriptor libre.

---

## 14. Sin rate limiting — Baja (diseño)

No existe ninguna limitación de tasa en el framework. El único control es
`max_connections` (10 000 por defecto, [app.hpp:320](include/lux/app.hpp#L320)), que acota
conexiones simultáneas pero no peticiones por IP ni por unidad de tiempo.

Para un framework que trae sesiones, JWT y hashing de contraseñas de serie, no ofrecer
nada contra fuerza bruta en un login es una laguna notable. Los timeouts de Slowloris
([http_connection.cpp:78-90](src/http/http_connection.cpp#L78-L90)) y el límite de pipelining
están bien resueltos; esto es lo que falta al lado.

---

## 15. `/metrics` y health sin autenticación — Informativa

**Dónde:** [app.hpp:300-316](include/lux/app.hpp#L300-L316).

`enable_metrics()` registra el endpoint Prometheus sin ningún guardia. Van por el router, así
que un middleware global puede protegerlos, pero el default los deja abiertos y exponen
volumen de tráfico y distribución de códigos de estado.

---

## 16. La VM no comprueba límites de pila ni de locals — Informativa

**Dónde:** [vm.hpp:92-93](include/lux_script/vm.hpp#L92-L93).

```cpp
void  push(Value v) { stack_.push_back(std::move(v)); }
Value pop()         { Value v = std::move(stack_.back()); stack_.pop_back(); return v; }
```

`pop()` no comprueba que la pila no esté vacía, y los accesos a
`locals_[frame.locals_base + in.operand]` ([vm.cpp:221,225](src/lux_script/vm.cpp#L221))
no verifican rango. La seguridad depende por completo de que el emisor genere bytecode
balanceado.

No es explotable desde la red —el bytecode se compila desde el `.lux` del propio autor, no
llega del atacante—, pero significa que cualquier bug del emisor se convierte en corrupción
de memoria en vez de en un error de lenguaje. Un `assert` en builds de debug, o un
verificador de bytecode al cargar, contendría la clase entera de fallos.

Relacionado: `kStepLimit` (50 M) se reinicia **en cada suspensión**
([vm.hpp:65](include/lux_script/vm.hpp#L65)), así que un bucle que haga `await` dentro nunca
lo agota. Es deliberado (SSE de larga duración), pero deja el corte sin efecto justo en los
bucles que hacen E/S.

---

## 17. Comentario falso sobre `fd_` tras `close()` — Informativa

**Dónde:** [http_connection.cpp:233-236](src/http/http_connection.cpp#L233-L236).

> «After close(), fd_ is -1, so calls degrade to write(-1) which returns EBADF cleanly.»

`fd_` **nunca** se pone a -1: `close()` ([http_connection.cpp:655](src/http/http_connection.cpp#L655))
hace `::close(fd_)` y deja el valor intacto. Sólo `file_fd_`, `header_tfd_` y `timeout_tfd_`
se resetean.

Hoy no es explotable porque el lambda `_raw_write` comprueba `closed_` antes de escribir, y
esa bandera se pone antes del `::close()`. Pero el comentario documenta como red de
seguridad algo que no existe: si alguien añade una ruta de escritura que confíe en el
comentario en lugar de comprobar `closed_`, escribirá sobre un descriptor que el kernel ya
pudo reasignar a otra conexión.

---

## Lo que está bien resuelto

Vale la pena dejar constancia de lo que aguantó la revisión, porque es bastante:

- **SQL.** Los tres drivers parametrizan de verdad: `sqlite3_bind_*` con `SQLITE_TRANSIENT`
  ([db_sqlite.cpp:214-227](src/lux_script/db_sqlite.cpp#L214-L227)) y `PQexecParams`
  ([db_postgres.cpp:302](src/lux_script/db_postgres.cpp#L302)). El reescritor de `?`→`$n`
  de Postgres salta correctamente literales, identificadores entre comillas, comentarios de
  línea, comentarios de bloque anidados y **dollar-quoting** (`$tag$...$tag$`) — que es el
  caso que casi todo el mundo olvida. `sqlite3_prepare_v2` con tail `nullptr` además impide
  stacked queries.
- **SSTI.** `render()` exige el nombre de plantilla literal en tiempo de compilación
  ([emitter.cpp:1435-1444](src/lux_script/emitter.cpp#L1435-L1444)), lo que elimina de raíz
  la clase entera de plantilla dinámica controlada por el atacante.
- **Parser JSON.** Tope de anidamiento de 200, rechazo de caracteres de control sin escapar,
  validación completa de pares suplentes y rechazo de basura al final del documento.
- **WebSocket.** Bits RSV, opcodes reservados, máscara obligatoria cliente→servidor, bit alto
  de la longitud de 64 bits, fragmentación intercalada y tamaño de frames de control: todas
  las reglas del RFC 6455 que se suelen saltar están comprobadas. Y `origins(...)` es
  **obligatorio** en rutas ws con error de compilación si falta
  ([project.cpp:1371-1373](src/lux_script/project.cpp#L1371-L1373)) — más estricto que la
  mayoría de frameworks, que lo dejan opcional.
- **`os.run()`** usa `posix_spawnp` con vector argv, nunca un shell: la inyección de
  metacaracteres no es posible por construcción.
- **Traversal en estáticos.** Doble canonicalización (antes y después de resolver symlinks),
  comparación de prefijo **por componentes** (no por cadena, así `/srv/www-evil` no pasa por
  `/srv/www`), bloqueo de dotfiles sobre la ruta ya decodificada y rechazo de `%00`.
  El hallazgo 3 es un fallo de implementación dentro de un diseño correcto.
- **Pipelining HTTP.** La serialización con pausa/resume del parser y el bracket `in_parser_`
  están razonados con cuidado y resuelven una reentrada real y sutil.
- **HMAC/sesión.** RFC 2104 correcto, comparación en tiempo constante, y el comentario sobre
  por qué se puede ramificar sobre `exp` *después* de verificar el MAC es acertado.

---

## Orden sugerido

1. **Hallazgo 1** — es el único con impacto de caída total remota y no autenticada, y afecta
   a cualquier app que use el módulo para lo que existe. Las opciones pasan por acotar el
   tamaño del sujeto antes de llamar a `std::regex`, mover el módulo a `is_async` con
   presupuesto de tiempo, o cambiar a un motor sin backtracking (RE2).
2. **Hallazgos 2 y 3** — un differential entre backends y un SEGV alcanzable.
3. **Hallazgo 4** — corrección pequeña y contenida en la validación de JWT.
4. El resto, por orden de la tabla.

---

## Estado de la remediación

Todo lo de aquí abajo vive en la rama `security-fixes`, compila limpio con `./compile.sh`
y pasa la suite existente (`ctest`) sin regresiones nuevas — los dos fallos que quedan
(`placeholders`/`faltan argumentos` y `http`/`invalid url`+`connection refused`) ya
fallaban en `d10f274` antes de tocar nada, confirmado recompilando esa misma revisión y
volviendo a correrlos.

### Corregidos

- **#1 — `regex.*`.** [module_regex.cpp](src/lux_script/module_regex.cpp) gana un tope de
  4096 bytes sobre el texto SUJETO (nunca el patrón) en las seis funciones, devuelto como
  un `error` normal en vez de dejar que `std::regex` recurse hasta reventar la pila. El
  valor sale de medir el punto de caída real con ASan a distintos tamaños de pila (ver el
  comentario junto a `kMaxSubjectLength`), con margen para pilas de hilo más pequeñas que
  los 8 MB por defecto de glibc. Verificado end-to-end: un body de 30 KB que antes tumbaba
  el proceso entero ahora responde 500 con un mensaje claro y el servidor sigue sirviendo
  el resto de conexiones. **No queda resuelto** el colgado por backtracking catastrófico
  sobre un patrón ya complejo dentro del límite (`^(a|aa)+$`): eso necesitaría dejar
  `std::regex` por un motor sin backtracking (RE2), fuera del alcance de este parche.
- **#2 — divergencia `--native`/bytecode.** [native_gen.cpp](src/lux_script/native_gen.cpp)
  reescribió `lux_route_coerce_int`/`lux_route_coerce_float` generados para exigir
  consumir la cadena entera (como ya hacía `project.cpp`) y rechazar formas no decimales.
  Verificado con una tabla de casos cruzada entre ambas rutas (`12abc`, `0x10`, `nan`,
  `inf`, `1e999`, …): coinciden en los 13 casos.
- **#3 — SEGV en estáticos.** [app.cpp](src/app.cpp) reemplaza los tres usos de
  `std::mismatch(root.begin(), root.end(), candidate.begin())` por `path_is_within()`,
  que avanza los dos iteradores en paralelo y nunca desreferencia más allá de
  `candidate.end()`. Verificado con ASan sobre el caso exacto que crasheaba (`root` más
  profundo que `candidate`) y con el binario real vía un symlink que colapsa a un
  directorio más corto que la raíz servida: ahora devuelve 403 y el proceso sigue vivo.
- **#4 — `verify_jwt` sin `iss`.** [auth.cpp](src/lux_script/auth.cpp): con un issuer
  configurado, un token sin el claim `iss` se rechaza igual que uno con el issuer
  equivocado, en vez de colarse por omisión.
- **#5 — `http.*` sin límite de respuesta.** [module_http.cpp](src/lux_script/module_http.cpp)
  acota cuerpo (16 MB, igual que el límite de entrada) y cabeceras (64 KB) de la
  respuesta; superarlo aborta la transferencia vía el mecanismo estándar de libcurl
  (devolver menos bytes de los recibidos en el callback) en vez de acumular sin límite.
- **#6 — CRLF en cabeceras salientes de `http.*`.** Mismo fichero: clave y valor de cada
  cabecera pasan por un `strip` de CR/LF/NUL antes de `curl_slist_append`, igual que ya
  hacían `Response::header()` y `build_set_cookie()` del lado de entrada. De paso,
  `CURLOPT_PROTOCOLS`/`CURLOPT_REDIR_PROTOCOLS` quedan fijados a `http,https` (forma en
  bitmask, no la `_STR` de curl 7.85+, para no romper la build con un libcurl-dev más
  viejo) — un 30x ya no puede llevar la petición a `file://` u otro esquema.
- **#7 — `read_buf` de WebSocket sin límite.** [websocket.hpp](include/lux/websocket.hpp):
  `feed()` deja de acumular bytes en cuanto `closed` es `true`, y las dos rutas que lo
  marcaban sin avisar (frame de más de 16 MB, cola de mensajes llena) ahora mandan un
  Close real (1009/1008) igual que el resto de violaciones del protocolo, y liberan
  `read_buf` en el acto en vez de esperar a que el objeto se destruya.
- **#8 — `float` acepta `nan`/`inf`/hex.** [project.cpp](src/lux_script/project.cpp) y
  [native_gen.cpp](src/lux_script/native_gen.cpp): antes de `std::stod`, se rechaza
  cualquier texto que contenga `x`/`X`/`n`/`N`/`i`/`I` — ningún float decimal finito
  puede llevar esos caracteres, así que el filtro no tiene falsos positivos. Mismo cambio
  replicado en ambas rutas para no reabrir el hallazgo 2.
- **#11 — `Content-Length`/`Transfer-Encoding` sobrescribibles.**
  [response.hpp](include/lux/response.hpp): `Response::header()` ignora (con aviso por
  stderr) cualquier intento de fijar esas dos cabeceras desde un handler — las calcula el
  framework y nunca hay un uso legítimo para que un handler las toque, ya que Lux no
  emite `chunked`.
- **#12 — inyección de fórmulas en `csv.*`.** [module_csv.cpp](src/lux_script/module_csv.cpp):
  un campo que empieza por `=`, `+`, `-`, `@`, TAB o CR se antepone con `'` (mitigación
  estándar de OWASP) antes del escapado RFC 4180.
- **#13 — `random_bytes` falla en silencio.** [crypto.cpp](src/lux_script/crypto.cpp) pasó
  de abrir `/dev/urandom` a `getrandom(2)` (no depende de un descriptor libre); sigue
  devolviendo `""` si falla, pero ahora el único consumidor que no comprobaba ese caso
  —el sufijo anticolisión de `File.save()`, [natives.cpp](src/lux_script/natives.cpp)—
  también lo hace, fallando alto en vez de reintentar con un nombre sin entropía real.
- **#17 — comentario falso sobre `fd_`.** [http_connection.cpp](src/http/http_connection.cpp):
  el comentario ahora dice lo que de verdad evita escribir tras `close()` (la comprobación
  de `closed_`, no que `fd_` valga -1, que nunca lo vale).

### Parcial

- **#9 — SSRF.** Se añadió la restricción de protocolo en redirects (ver #6), que cierra
  la vía más barata de escapar del `http://`/`https://` inicial. **No** se añadió bloqueo
  de rangos privados/loopback/link-local: hacerlo bien requiere resolver el DNS y
  comprobar la IP resultante (no solo mirar el string de la URL, que un atacante rodea con
  un dominio que resuelve a `127.0.0.1`), y decidir si es opt-in u opt-out afecta a
  cualquier despliegue que llame a un servicio interno legítimo desde una ruta. Es una
  decisión de producto, no un bug con una corrección obvia — lo dejo para que el
  mantenedor decida la política antes de imponerla.

### No tocados (decisión de diseño, no bug)

- **#10 — estáticos saltan el middleware.** Es un comportamiento documentado explícitamente
  en el propio código (`app.cpp`). Cambiarlo altera el contrato del framework para toda
  app existente que dependa de ese orden; no es algo que deba decidir yo.
- **#14 — sin rate limiting.** Es una funcionalidad que falta, no un defecto en código
  existente. Añadir una encaja mejor como una propuesta aparte con su propio diseño
  (por IP, por sesión, ventana fija vs. token bucket, etc.) que como parte de una tanda de
  fixes.
- **#15 — `/metrics` sin autenticación.** Mismo dato expuesto que Prometheus expone por
  convención en la inmensa mayoría de despliegues (protegido por red o por el propio
  scraper); forzar auth por defecto rompería cualquier `prometheus.yml` existente sin un
  flag de opt-out que habría que diseñar primero.
- **#16 — la VM no comprueba límites de pila/locals.** Solo es alcanzable si el propio
  emisor de Lux genera bytecode mal formado, no desde una petición HTTP. Añadir
  comprobaciones (`assert`, que aquí no se usa en ningún otro sitio del código) a costa de
  la ruta más caliente del intérprete es un cambio que vale la pena medir en `bench/`
  antes de aplicarlo a ciegas, no algo para colar sin datos de por medio.
