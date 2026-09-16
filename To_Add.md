# To add

Limitaciones reales del compilador/lenguaje encontradas de pasada mientras se trabajaba
en otra cosa — no bloquean lo que se estaba haciendo en el momento (siempre hay un
workaround razonable), pero merece la pena arreglarlas más adelante. Cada entrada dice
qué falla, por qué, el workaround actual, y dónde mirar para arreglarlo de verdad.

---

## `type_of()` no conoce el tipo de retorno de una llamada a función de usuario — ARREGLADO

**Encontrado:** 2026-09-16, mientras se arreglaba que un `fn` suelto no podía usar
constructores de clase (ver el commit "Standalone functions can now use classes").

**Arreglado:** mismo día, commit "Resolve chained method calls on function/constructor
results". `FnSig` ganó un campo `devuelve` (el tipo de retorno declarado), rellenado por
`make_sig()`, y `type_of()`/`check_call` lo consultan en el caso `ExprKind::Call` (tanto
para una función suelta como para un método de clase) en vez de devolver `Type::unknown()`.
`check_call`'s propia resolución del receptor de un `.método()` (antes un chequeo a mano
solo para `Ident`/`this`) pasó a usar `type_of()` también, que es lo que de verdad
resuelve el ejemplo de abajo. Verificado con 60 peticiones seguidas contra un proceso
limpio (sin nada más escuchando en el puerto) — ver la nota al final sobre el
falso-positivo de no-determinismo que salió al verificarlo.

**Nota sobre un no-determinismo que resultó ser falso:** durante la verificación, el mismo
repro daba 204 en vez de `{"r":25}" en una fracción de las peticiones, de forma
aparentemente aleatoria, incluso con concurrencia y con ThreadSanitizer sin quejarse.
Resultó ser un artefacto del entorno de pruebas, no un bug: `SO_REUSEPORT` permite que
más de un proceso escuche el mismo puerto a la vez, y un proceso de la extensión de
VSCode de otro repo (`.../Github/Lux/build/lux .`) llevaba un rato escuchando por
casualidad en el mismo puerto (8098) usado para este repro de prueba en `/tmp`. El
kernel repartía las conexiones nuevas entre los dos procesos (pegajoso por conexión,
de ahí que pareciera "a veces sí, a veces no" pero siempre igual dentro de la misma
conexión) — el proceso ajeno no tiene esa ruta y devolvía 204 sin más. Confirmado
matando ambos procesos y repitiendo la prueba en un puerto verificado como libre:
60/60 peticiones correctas. Moraleja para la próxima vez que algo parezca no
determinista en un puerto reusado entre pruebas: `ss -tlnp | grep <puerto>` primero,
no asumir que solo el proceso propio está escuchando ahí.

**Qué falla:**

```lux
class Punto:
    int x
    int y

    fn int cuadrado():
        return this.x * this.x + this.y * this.y

fn Punto hacer_punto(int x, int y):
    return Punto(x, y)

get endpoint("/x"):
    return { "r": hacer_punto(3, 4).cuadrado() }
```

Compila (`lux --check` no se queja), pero falla en tiempo de ejecución:

```
{"error":"Dicts have no method 'cuadrado'"}
```

**Por qué:** `Emitter::type_of()` (`src/lux_script/emitter.cpp`) solo sabe el tipo de una
expresión `Call` cuando es una llamada a MÉTODO sobre un receptor de tipo ya conocido
(`case ExprKind::Call: { if (!e.object || e.object->kind != ExprKind::Member) return
Type::unknown(); ... }`). Para una llamada a una función de usuario suelta
(`hacer_punto(3, 4)`), no hay ninguna rama que consulte `FunctionSigs`/el tipo de retorno
declarado de la función — cae directo a `Type::unknown()`. Sin saber que el resultado es
un `Punto`, el checker no puede resolver `.cuadrado()` sobre él, y en tiempo de
ejecución el valor (un `Value::Dict` normal y corriente) se despacha por la vía dinámica
genérica, que solo conoce los métodos de `Dict` (`has`/`keys`), de ahí el mensaje.

Confirmado que es **independiente** de si la llamada ocurre dentro de una ruta o dentro
de otro `fn` — falla igual en los dos sitios, así que no tiene relación con el fix de
`build_function_signatures()`/`emit_function_bodies()` que sí se hizo.

**Workaround actual (funciona perfectamente):** asignar el resultado a una variable
local con el tipo declarado antes de encadenar el método:

```lux
fn Punto hacer_punto(int x, int y):
    return Punto(x, y)

get endpoint("/x"):
    Punto p = hacer_punto(3, 4)
    return { "r": p.cuadrado() }
```

Esto funciona porque `type_of(Ident)` sí devuelve el tipo declarado de la variable
(`local_type(e.text)`), a diferencia de `type_of(Call)`.

**Dónde mirar para arreglarlo:** `Emitter::type_of()`, caso `ExprKind::Call`
(`src/lux_script/emitter.cpp`, cerca de la línea 538 en el commit donde se encontró esto).
Necesitaría una rama nueva: si `e.object->kind == ExprKind::Ident` y ese nombre resuelve
en `functions_` (la `FunctionSigs` que el `Emitter` ya recibe), devolver el tipo de
retorno declarado de esa función — análogo a como ya se resuelve el retorno de un
método (`m.devuelve ? Type::from_legacy_name(m.devuelve) : recv`), pero mirando
`FnSig`/lo que sea que guarde el tipo de retorno de una función suelta en vez de un
método. Comprobar también si `check_call`/`emit_call` tienen el mismo tipo de hueco
para otras formas encadenadas (`fn_que_devuelve_list()[0].campo`, etc.) una vez se
toque esto, no solo el caso de un método.
