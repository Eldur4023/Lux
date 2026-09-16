# To add

Limitaciones reales del compilador/lenguaje encontradas de pasada mientras se trabajaba
en otra cosa — no bloquean lo que se estaba haciendo en el momento (siempre hay un
workaround razonable), pero merece la pena arreglarlas más adelante. Cada entrada dice
qué falla, por qué, el workaround actual, y dónde mirar para arreglarlo de verdad.

---

## `type_of()` no conoce el tipo de retorno de una llamada a función de usuario

**Encontrado:** 2026-09-16, mientras se arreglaba que un `fn` suelto no podía usar
constructores de clase (ver el commit "Standalone functions can now use classes").

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
