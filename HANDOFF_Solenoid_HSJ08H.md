# Handoff Técnico — Stage 5: Librería de control del solenoide (HSJ08H)

> Documento de transferencia para continuar el desarrollo en un nuevo chat.
> Proyecto: ES242F smart-lock, retrofit ESP32-S3 (ver `PROJECT_NOTES.md`).
> Stage 5 (solenoide del abrepuertas) en arranque. Fecha: 2026-08-15.
> Fuentes: circuito levantado por el usuario (`hbridge_schem.jpeg`), datasheet
> MX608E (equivalente pin-a-pin), plan de evidencia acordado (D4).
> Decisiones que aplican: D2 (paridad OEM), D4 (evidence-first), D11
> (toolchain ESP-IDF v6.1-dev-6940-g08e0d30a74a pineado).

---

## 1. Objetivo de la etapa

Construir `components/lockdrv/` (API pública en `include/`, implementación en
`src/`, harness de consola estilo `audio_test` en `main/`) que accione el
solenoide del abrepuertas con **paridad OEM** (D2) y protecciones (pulse
timeout, anti-rebote, no-retrigger).

Orden de trabajo acordado:

1. **Interpretar el circuito** del driver HSJ08H — ✅ HECHO (2026-08-15, §2–§4).
2. **Caracterizar el comportamiento OEM con el osciloscopio** — ancho y forma
   del pulso de apertura, tensión y corriente del solenoide, secuencia
   respecto a eventos (verify OK, tecla, apertura remota Tuya). ⬜ PENDIENTE.
3. **Diseñar la librería** (driver + API) con paridad de timings y
   protecciones. ⬜ PENDIENTE (borrador en §7).
4. **Harness de banco** estilo `audio_test` para validar antes de integrar.
   ⬜ PENDIENTE.

---

## 2. Identificación del driver — RESUELTO (evidencia de mercado asiático)

El **HSJ08** (marcaje en placa: "HSJ08H 412AX1") es un **puente H de canal
único en SOP-8, específico para cerraduras electrónicas**. No tiene datasheet
público propio, pero existe una familia de compatibles pin-a-pin:

| Marca | Chip | Fuente |
|---|---|---|
| Mixic (中科芯亿达) | **MX608E** — datasheet completo, REFERENCIA DE TRABAJO | LCSC C115187 |
| — | HR1124S | listados de compatibilidad |
| — | TC118S | listados de compatibilidad |
| — | YX9020AM | listados de compatibilidad |
| — | LGM9680 | listados de compatibilidad |

La cadena de evidencia: listados de Alibaba/Taobao venden el MX608E
explícitamente como "reemplazo del HSJ08" para drivers de cerradura; artículos
de la industria (Sohu, RFIDWorld, foro EETOP) listan la familia de
compatibles; y el circuito levantado en placa coincide **pin por pin** con el
pinout del datasheet del MX608E (§3).

> **Confirmación pendiente (D4):** la identidad se considera validada cuando
> la tabla de verdad medida con scope (§6) coincida con la del MX608E.

### Especificaciones clave (datasheet MX608E)

| Parámetro | Valor | Nota |
|---|---|---|
| Estructura | Puente H, MOSFETs N y P integrados | Apto cargas inductivas (bobinas, motores) |
| VDD (potencia) | 2.0 – 9.6 V | En placa: VBAT = 4×AA ≈ 6 V |
| VCC (lógica) | 1.8 – 5 V | En placa: +3V3 |
| Corriente de salida | 1.1 A continua / 1.5 A pico | @ VDD=6.5 V, 27 °C |
| RDS(on) | ~0.44–0.48 Ω | |
| Standby | < 0.1 µA | INA=INB=L (coast) |
| Entradas | Pull-down interno ~15 kΩ; umbral alto > 2.4 V si VCC flotante | Seguro con CPU en reset/deep-sleep |
| Protecciones | TSD 150 °C con histéresis, anti shoot-through (conducción cruzada), protección de motor trabado, freno activo | |
| ESD | 3 kV HBM | |
| PWM en entradas | Soportado (datasheet especifica delays @ 20 kHz) | Habilita hold-current por PWM si el OEM lo usa |

Dato de aplicación relevante (nota de aplicación "electronic lock" de la
familia MX08): capacitor de 0.1 µF entre OUTA y OUTB junto a la bobina, y
desacople de VDD de 4.7–100 µF según corriente. En nuestra placa: C6 entre
salidas y C20 en VBAT — coinciden.

### Tabla de verdad (MX608E)

| INA (SOL1) | INB (SOL2) | OUTA | OUTB | Estado |
|---|---|---|---|---|
| L | L | Z | Z | **Standby / coast** (alta impedancia) |
| H | L | H | L | **Forward** |
| L | H | L | H | **Reverse** |
| H | H | L | L | **Brake** (ambos low-side ON, cortocircuita la bobina) |

---

## 3. Circuito levantado (`hbridge_schem.jpeg`) — interpretación

Mapeo pin a pin (verificado contra datasheet MX608E):

| Pin | Función | Net en placa | Evidencia en el esquema |
|---|---|---|---|
| 1 | VCC lógica | +3V3 | C7 a GND (desacople lógico) |
| 2 | INA | SOL1 ← FR8018H **PA4** (función alt: PWM4) | entrada de control |
| 3 | INB | SOL2 ← FR8018H **PA5** (función alt: PWM5) | entrada de control |
| 4 | VDD potencia | VBAT (4×AA ≈ 6 V) | C20 a GND (desacople potencia) |
| 5 | OUTB | nodo salida inferior → conector | |
| 6 | GND | GND | |
| 7 | GND | GND | |
| 8 | OUTA | nodo salida superior → conector | |

Otros componentes del circuito:

| Ref | Interpretación | Confianza / cómo confirmar |
|---|---|---|
| **C6** (entre OUTA y OUTB) | Capacitor de EMI en paralelo con la bobina (~0.1 µF, práctica estándar; la nota de aplicación de la familia lo menciona) | Alta. Medir valor si se desea |
| **D8 / D9** (a GND, en el conector) | Casi seguro **TVS unidireccionales** (protección ESD/surge del cable que sale de la cerradura). NO son necesarios para el flyback: el puente H ya integra los diodos de recirculación (body diodes) | Media. Confirmar leyendo el marcaje del encapsulado o midiendo Vbr con fuente limitada |
| **R05D** (serie con la bobina) | Si es "R050" = 0.05 Ω: candidato a **shunt de corriente** (¿hay un ADC del FR8018H mirando ese nodo?) o fusible/PTC | Baja. Verificar marcaje y continuidad hacia pads del CPU |
| **J1** | Probablemente el **conector de 2 pines** hacia el abrepuertas (no se trazó bobina de excitación de relé) | Media. Confirmar en placa |
| **"UE6RD"** | Posible marcaje del solenoide/cable | Baja. Verificar |

---

## 4. Hipótesis de control (a resolver con scope)

Descartadas por el análisis del circuito:

- ~~**Doblador/elevador de tensión**~~ — no hay boost: el puente H aplica
  **VBAT directamente** a la bobina. Su propósito es **invertir polaridad**.
- ~~**Dos PWM en contrafase**~~ — SOL1/SOL2 son entradas de **dirección** con
  lógica estática (tabla de verdad §2), no fases complementarias. (PWM sobre
  una entrada es posible y está soportado, pero sería para hold-current, no
  para sintetizar tensión.)

Hipótesis vivas sobre el actuador (foto `solenoid.jpeg`: solenoide encapsulado
negro, 2 cables rojo/negro, conector JST, disponible en el banco para observar
el accionamiento mecánico):

- **(a) Solenoide biestable (latching magnético):** pulso en una polaridad
  abre, pulso en la polaridad opuesta cierra/rearma. Consumo cero en reposo —
  muy común en cerraduras a pilas. Explicaría la elección de un puente H.
- **(b) Solenoide unipolar con retorno por resorte:** el OEM usa un solo
  sentido (forward) y el puente H es simplemente el driver más barato
  disponible; el estado HH (brake) podría usarse para disipar la energía de
  la bobina al soltar.

Mediciones que deciden: polaridad(es) usadas, ancho de pulso, PWM de
mantención, estado al final del pulso (coast vs brake), y observación
mecánica directa del actuador en el banco.

---

## 5. Preparación del banco (usuario)

- [ ] Reinstalar la placa de la cerradura en el banco de trabajo.
- [ ] Soldar cables de medición a INA (pin 2) e INB (pin 3) del HSJ08H.
- [ ] Puntos adicionales si es posible: OUTA (pin 8), OUTB (pin 5) o
      directamente sobre el conector del solenoide; nodo de R05D para corriente.
- [ ] Dar de alta una tarjeta Mifare (para disparar apertura por tarjeta).
- [ ] Tener el solenoide actuador a la vista para observar el movimiento.

## 6. Plan de capturas (scope, RIGOL DHO924S)

Nombrado según convención: `sol_<evento>_<señal>.png`.

| # | Captura | Canales | Evento disparador |
|---|---|---|---|
| 1 | Entradas de control | CH1=INA, CH2=INB | Apertura por **huella OK** |
| 2 | Ídem | ídem | Apertura por **código de teclado OK** |
| 3 | Ídem | ídem | Apertura **remota Tuya** |
| 4 | Salida sobre la bobina | CH1=OUTA, CH2=OUTB + MATH=CH1−CH2 | Cualquiera de los anteriores |
| 5 | Corriente de la bobina | shunt R05D o pinza | Ídem (pull-in, pico, posible PWM) |
| 6 | Zoom al final del pulso | MATH diferencial | Detectar coast (decaimiento lento por body diodes) vs brake (decaimiento rápido) |
| 7 | Secuencia larga | INA+INB | ¿Pulso inverso de rearme/cierre? ¿Cuándo? |

Preguntas que debe responder la sesión:

1. ¿Ancho del pulso de apertura? ¿Igual para los tres eventos?
2. ¿Una sola polaridad o forward+reverse (biestable)?
3. ¿Hay PWM de hold-current después del pull-in?
4. ¿Fin del pulso en coast (LL) o brake (HH)?
5. ¿Tensión y corriente de la bobina (pico y plateau)?
6. ¿Qué es R05D? ¿Qué es J1? ¿Marcaje de D8/D9?

---

## 7. Diseño preliminar de la librería `lockdrv` (BORRADOR — congelar tras §6)

Decisiones de diseño ya tomadas (independientes de las mediciones):

- **Estado idle seguro:** INA=INB=LOW (coast, < 0.1 µA, pull-downs internos de
  15 kΩ lo mantienen seguro incluso con el CPU en reset). Todo path de error
  vuelve a este estado.
- **Protecciones de la API** (pedidas por el usuario):
  - `pulse timeout`: ninguna entrada puede quedar en HIGH más allá del ancho
    de pulso OEM medido (timer de hardware, no delays de software).
  - `anti-rebote / no-retrigger`: triggers durante el pulso activo se
    descartan (o encolan, a decidir); guard time post-pulso.
- **Parámetros en `Kconfig` del componente** (D11-bis: nada de flags por
  línea de comando): ancho de pulso, guard time, polaridad, modo de release.

Decisiones que DEPENDEN de las mediciones:

| Medición | Si sale A | Si sale B |
|---|---|---|
| Polaridad | unipolar → API solo `open()` | biestable → API `open()` + `close()` (polaridades opuestas) |
| Fin de pulso | coast → `release_mode = COAST` | brake → `release_mode = BRAKE` |
| Hold-current | pulso DC simple (GPIO + timer) | PWM post-pull-in (LEDC/MCPWM) |

API tentativa (a revisar):

```c
typedef struct {
    gpio_num_t ina_pin;      /* SOL1 (ex FR8018H PA4) */
    gpio_num_t inb_pin;      /* SOL2 (ex FR8018H PA5) */
    uint32_t   pulse_ms;     /* OEM pulse width (Kconfig default) */
    uint32_t   guard_ms;     /* no-retrigger window after pulse */
} lockdrv_config_t;

esp_err_t lockdrv_init(const lockdrv_config_t *cfg);
esp_err_t lockdrv_open(void);            /* pulse, OEM polarity */
esp_err_t lockdrv_close(void);           /* only if strike is latching */
esp_err_t lockdrv_emergency_off(void);   /* force coast from any state */
bool      lockdrv_is_busy(void);
```

Harness `main/` estilo `audio_test`: comandos de consola `open`, `close`,
`pulse <ms>`, `sweep <ms_min> <ms_max>` (barrido de ancho para márgenes),
`stress <n>` (n aperturas con guard time, verifica no-retrigger y TSD).

---

## 8. Criterios de aceptación de la etapa (a ejecutar en banco)

1. Tabla de verdad del driver medida = tabla MX608E (confirma identidad, D4).
2. Pulso generado por `lockdrv` indistinguible del OEM en scope (ancho,
   polaridad, forma, fin de pulso) para los 3 eventos.
3. Pulse timeout: ninguna condición de error deja la bobina energizada.
4. No-retrigger: ráfaga de triggers durante pulso + guard = 1 solo pulso.
5. Consumo en idle del driver < 1 µA (coast + pull-downs).
6. Harness documentado en README del componente con capturas.

---

## 9. Fuentes

- Datasheet MX608E (Mixic/中科芯亿达), LCSC C115187:
  https://www.lcsc.com/product-detail/Brushed-DC-Motor-Drivers_Mixic-MX608E_C115187.html
- Nota de aplicación "electronic lock" familia MX08 (ChipSourceTek):
  https://en.chipsourcetek.com/Driver-Chip/2608.html
- Listados de compatibilidad HSJ08 (Alibaba/Taobao/Sohu/RFIDWorld/EETOP):
  MX608E como reemplazo pin-a-pin del HSJ08 para cerraduras electrónicas.
- Datasheet FR8018H (`FR8018H_CPU.pdf`, repo del proyecto): PA4/PA5 con
  funciones alternativas PWM4/PWM5.

---

## 10. RESULTADOS — El actuador es un MOTOR DC con caja reductora (2026-08-15) ✅

### Capturas (RIGOL DHO924S, evento: apertura por tarjeta habilitada)

| Captura | Canales | Qué muestra |
|---|---|---|
| `lockdrv_open_pulse_235mS.png` | CH1=INB, CH2=INA | Pulso de apertura: **INA=H durante 235.5 ms** (ΔX cursores) |
| `lockdrv_open_close_pulses_4.8Seg.png` | CH1=INB, CH2=INA | Ciclo completo: pulso INA, y **4.82 s después** (ΔX entre flancos) pulso INB de 235 ms |
| `lockdrv_outa_outb.png` | CH1=INB, CH2=INA, CH3=OUTA, CH4=OUTB | Apertura: **OUTA=H / OUTB=L**; cierre: **OUTB=H / OUTA=L** |

### Hechos medidos (evidence-first, D4)

1. **El actuador NO es un solenoide**: es un **motor DC reversible con caja
   reductora** (sonido de reductora audible durante todo el pulso; el pestillo
   se observa salir y entrar). Control **en lazo abierto por tiempo** — no se
   trazaron fines de carrera.
2. **Apertura:** INA=H ~235 ms → OUTA=H, OUTB=L → el pestillo **sale**.
3. **Re-cierre automático:** ~4.8 s después (4.58 s desde el fin del pulso de
   apertura), INB=H ~235 ms → OUTB=H, OUTA=L → el pestillo **entra** (reposo).
   El dwell de ~4.8 s lo genera el firmware del CPU → pertenece a `lock_app`,
   no a la librería.
4. **Sin PWM ni hold-current:** pulsos DC planos al nivel de alimentación
   (~4.6 V medidos con la placa alimentada por USB; pendiente re-medir a
   batería ~6 V).
5. **Release = coast:** después de cada pulso ambas entradas vuelven a LOW
   (Hi-Z, standby < 0.1 µA). **No se observó brake (HH).**
6. **Identidad del driver confirmada en el cable (D4 ✅):** forward y reverse
   observados, coinciden con la tabla de verdad del MX608E.
7. Nivel alto de INA/INB ≈ 3.3 V (lógica del FR8018H) — directamente
   compatible con GPIO del ESP32-S3.

### Interpretación funcional

Reposo = pestillo retraído; verify-OK lo extiende por una ventana de ~4.6 s y
luego se retrae solo. Es el esquema clásico de **clutch-pin motorizado** de
estas cerraduras (el pin acopla el mango al mecanismo durante una ventana de
apertura temporizada) — a confirmar observando la mecánica en el banco.

### Decisiones de diseño destrabadas (actualiza §7)

| Pregunta | Resolución medida |
|---|---|
| ¿Polaridad? | **Bipolar** → la API necesita `open()` (INA) y `close()` (INB) |
| ¿Fin de pulso? | **COAST** (LL) → `release_mode` fijo, sin brake |
| ¿Hold-current? | **No hay PWM** → driver con GPIO + timer (gptimer), sin LEDC/MCPWM |
| ¿Dwell 4.8 s? | Es de aplicación → `lock_app` (paridad: 4.58–4.82 s) |
| ¿Ancho de pulso? | **235 ms** → default `CONFIG_LOCKDRV_PULSE_MS=235` |

### Pendientes para la próxima sesión de lab

- [ ] Mismas capturas para **tecla/código OK** y **apertura remota Tuya**
      (expectativa: pulsos idénticos, solo cambia el trigger del CPU).
- [ ] Zoom al fin del pulso: forma del decaimiento del flyback en coast.
- [ ] Corriente del motor (shunt R05D o pinza): pico de arranque y plateau.
- [ ] Identificar R05D (¿shunt? ¿PTC?), J1 (¿conector?) y marcaje de D8/D9.
- [ ] Re-medir ancho de pulso con alimentación a batería (~6 V): el timing de
      235 ms es del firmware (debería mantenerse); el tránsito del motor será
      más rápido.

---

## 11. Actuador identificado a nivel de clase + hipótesis de power-up (2026-08-15)

### Clase del actuador (evidencia de mercado asiático)

El actuador (`solenoid.jpeg`: caja plástica negra, orejas de montaje, 2 cables
rojo/negro, conector JST) pertenece a la clase **半自动指纹锁电机** = "motor
para cerradura de huella **semiautomática**". Son partes de commodities SIN
datasheet formal (igual que el HSJ08): las specs viven en los listados de
Taobao/Alibaba. Parámetros típicos de la clase:

| Parámetro | Valor típico de la clase | Fuente |
|---|---|---|
| Tensión de trabajo | DC 3–6 V (mainstream); algunos 6–12 V | listados Taobao |
| Corriente en movimiento | < 0.2 A típico | listados Taobao |
| Corriente de stall (estándar industria) | <= 800 mA | guías de reparación |
| Carrera del vástago | 8–16 mm según modelo (series D1–D19) | guías de reparación |
| Lógica de clutch | el motor energiza brevemente tras verificar; el retorno lo hace la mecánica/resorte | idem |

Referencia cruzada muy útil: un manual OEM de una cerradura semiautomática de
la misma clase (4 pilas alcalinas, 6 V) especifica **corriente máxima de
trabajo 250 mA** y tensión de drive del motor 4.5–6.5 V — consistente con lo
medido y con el margen del MX608E (1.1 A).

**Conclusión:** no hay datasheet que buscar; la caracterización de NUESTRA
unidad se hace por medición (corriente con shunt R05D o pinza — ya en el
checklist). La clase está identificada y es suficiente para el diseño.

### Observación del usuario: ruido de reductora en el power-up

Tras el power-up se escucha el mismo sonido de reductora → hipótesis: el
firmware OEM emite un **pulso de inicialización** al arrancar para llevar el
actuador a la posición de reposo conocida (indispensable al ser lazo abierto:
tras un corte de energía el CPU no sabe dónde quedó el pestillo).

- [ ] **Capturar INA/INB durante el power-up** (`sol_powerup_ina_inb.png`):
      ¿qué polaridad, qué ancho, cuánto después de energizar?
- Implicancia para `lockdrv`: `lockdrv_init()` deberá replicar la secuencia de
  boot del OEM (paridad D2) — p.ej. pulso de retract al inicializar.

---

## 12. RESULTADOS — Power-up + matriz de eventos + secuenciación de audio (2026-08-15) ✅

### Pulso de power-up (`lockdrv_power-up.png`, CH1=INB, CH2=INA, CH3=OUTA, CH4=OUTB)

1. **Confirmado:** al energizar, el OEM emite un pulso por **INB** (misma
   dirección que el cierre/retract) **aunque el vástago ya esté adentro** →
   "drive to known rest position". Sin sensor de posición y en lazo abierto,
   es la única forma de arrancar en un estado mecánico conocido.
2. **Ancho ≈ 290 ms** (estimación por píxel, ±30 ms) — aparentemente **más
   largo que los 235 ms operativos**, consistente con "garantizar llegar al
   tope desde cualquier posición inicial".
   - [ ] Confirmar con cursores: ¿el pulso de boot es intencionalmente más
         largo que el de cierre? Si sí → parámetro separado
         `CONFIG_LOCKDRV_BOOT_PULSE_MS`.
3. **Flyback visible:** tras el flanco de bajada de OUTB se ve una cola de
   decaimiento (no un corte abrupto) → confirma visualmente **release por
   coast** (la energía circula por los body diodes / resistencia del motor;
   un brake la habría cortado en seco).
4. **Hipótesis del usuario (embrague deslizante):** muy plausible — estos
   actuadores suelen tener slip clutch que patina en el tope para no romper
   la reductora. Verificable con la medición de corriente pendiente: un
   plateau de stall (orden 250–800 mA) durante el pulso de boot con el
   vástago ya en casa sería la firma del tope + embrague.

### Matriz de eventos — CERRADA

Apertura por **tarjeta Mifare = teclado = app Tuya**: pulsos idénticos
(verificado por el usuario; no se archivan capturas duplicadas). El evento
solo cambia el trigger del CPU, no la forma de onda.

### Secuenciación de audio alrededor del evento de apertura (para lock_app)

| Evento | Audio OEM observado |
|---|---|
| Apertura (vástago sale) | jingle; **a veces** seguido casi de inmediato por el mensaje "desbloqueado" — dependencia desconocida |
| Cierre (vástago entra, incl. auto-relock a los ~4.6 s) | **siempre** el mensaje "apagado" |

Los prompt IDs viven en el mapa del `HANDOFF_Audio_CS8302.md` (§4sex).
- [ ] Identificar de qué depende que suene "desbloqueado" tras el jingle
      (¿primera apertura tras boot? ¿configuración de voz? ¿tipo de
      credencial?). Anotar el contexto la próxima vez que ocurra.

---

## 13. RESULTADOS — Mediciones finales de caracterización (2026-08-16) ✅

### Ancho del pulso de boot — RESUELTO: es el operativo

Cursor sobre `lockdrv_powerup_240mS.png`: **ΔX = 240 ms**. El pulso de boot es
el mismo que el operativo (235–240 ms, dentro de tolerancia de medición).
**NO hay parámetro separado de boot**: un solo `PULSE_MS` cubre todo.
La estimación por píxel de ~290 ms quedó descartada (error de la estimación).

Bonus de la misma captura: la cola de decaimiento exponencial de OUTB tras el
fin del pulso se ve perfectamente → coast confirmado con buena evidencia.

### Alimentación (VBAT pin 4 del HSJ08H)

**VBAT = 5.007 V con alimentación USB** (`lockdrv_VBAT_5V_USB.png`) → **no hay
diodo serie de protección contra inversión** (no se pierden ~0.5 V; lógico en
un dispositivo a baterías). A batería real serán ~6 V: el motor transitará más
rápido pero el pulso es timing de firmware (se mantiene en 235–240 ms).

### Corriente del actuador (medición limitada, suficiente para el diseño)

Tester UT71E en mA RMS (`lockdrv_pulse_current.jpeg`): **MAX = 41.5 mA**.
Sin sonda de corriente ni integración conocida del instrumento no es un valor
exacto: si el multímetro integra en una ventana T, la lectura RMS de un pulso
de 0.24 s es I·sqrt(0.24/T) → para T entre 0.3 y 1 s, la corriente del pulso
está entre **~50 y ~90 mA**. Orden de magnitud: 10² mA.

Consecuencias para el diseño (suficientes, no hace falta más precisión):

* MX608E (1.1 A continua) tiene margen ×10 — el driver nunca es el límite.
* Costo energético por apertura: ~90 mA × 0.48 s ≈ 43 mA·s ≈ **12 µAh** —
  despreciable para el presupuesto de baterías (§6.3 de las notas).
* El valor bajo y estable es **consistente con el slip clutch**: el motor
  nunca se traba eléctricamente (el embrague patina en el tope), por eso no
  hay pico de stall que medir. Hipótesis del embrague: reforzada.

### R05D — qué es y qué relevancia tiene

* Es el **designador** del componente en serie con el cable del motor (del
  esquema levantado). Prefijo "R" → probablemente **resistencia SMD de bajo
  valor**; si el marcaje impreso es "R050" sería 0.05 Ω = **shunt de
  corriente**. Alternativa: PTC/fusible resettable (protección de stall).
* **Cómo identificarlo:** leer el marcaje con lupa o foto macro. Si es shunt,
  medir la tensión entre sus extremos con el scope durante el pulso daría la
  forma de corriente exacta (I = V/R) — mate dos pájaros de un tiro.
* **Relevancia para esta etapa: BAJA / no bloqueante.** La librería no depende
  de él (driver sobredimensionado ×10, corriente ~10² mA acotada). Queda como
  ítem de completitud, útil solo si se quiere la forma de corriente exacta.

### Estado de la caracterización OEM: COMPLETA para diseñar

Pendientes NO bloqueantes (nice-to-have): marcaje de R05D / J1 / D8-D9,
re-medición a batería ~6 V.

---

## 14. Librería `lockdrv` v1 + harness `lockdrv_test` ENTREGADOS (2026-08-16)

Diseño aprobado por el usuario e implementado. Entregable: `lockdrv_test.zip`
(proyecto standalone con `components/lockdrv/` vendored, mismo patrón que
`audio_test`). Para integrar: copiar `components/lockdrv/` al repo principal.

* **Arquitectura:** 2 GPIOs + 1 gptimer. El fin del pulso ocurre en la ISR de
  alarma del timer → un cuelgue de tarea NUNCA deja el motor energizado
  (pulse timeout por construcción). El guard window corre en esp_timer
  (política, no seguridad).
* **Máquina de estados:** `IDLE → PULSING → GUARD → IDLE`; re-triggers
  rechazados con `ESP_ERR_INVALID_STATE`. El brake (INA=INB=H) es inalcanzable
  por la API.
* **API:** `init / home / open / close / abort / is_busy / get_state /
  state_str` + hook de banco `lockdrv_test_raw_pulse()` (bypasea guard, para
  sweeps de margen).
* **Kconfig (D11):** `LOCKDRV_PULSE_MS=235` (paridad OEM), `LOCKDRV_GUARD_MS=100`,
  `LOCKDRV_HOME_ON_INIT=y`, `LOCKDRV_HOME_DELAY_MS=500` (NO medido en OEM —
  ajustar cuando se capture el timing de boot).
* **Comandos del harness:** `home`, `open`, `close`, `cycle [dwell_ms]`
  (default 4600 = auto-relock OEM), `pulse <ext|ret> <ms>`, `sweep`, `stress
  <n>` (prueba rechazo de retrigger), `abort`, `state`.
* **Cableado de banco:** GPIO7→SOL1(INA), GPIO8→SOL2(INB) (los cables de
  scope ya soldados), GND común, FR8018H en reset (GPIOs hi-Z). GPIO7/8 para
  no pisar el cableado de audio en GPIO4/5/6.
* **PENDIENTE:** validación en banco contra la lista de aceptación (README
  del harness) + build con el toolchain pineado (D11).

### Fix v1.1 (2026-08-16, primer build del usuario)

Error mío de tipeo: `GPTIMER_COUNT_DIRECTION_UP` → el enumerador correcto es
**`GPTIMER_COUNT_UP`** (válido en 5.x y 6.x; NO era un problema del toolchain
pineado). Además se agregó `esp_hal_gpio` al `REQUIRES` del componente
(ahí vive `hal/gpio_ll.h` en IDF 6.x). Verificado contra la guía de migración
6.0: el resto de las APIs usadas (`gptimer_*`, esp_timer) están vigentes —
lo removido en 6.0 fue solo el legacy timer group driver.

---

## 15. RESULTADOS — Validación de banco de `lockdrv` (2026-08-16) ✅ PASSED

Build + flash OK con el toolchain pineado (D11). Suite de aceptación completa
en el testlab (`lockdrv_testlab.png`): placa de la cerradura + ESP32-S3
inyectando en SOL1/SOL2 con el FR8018H en reset.

| Test | Resultado | Captura |
|---|---|---|
| `open` vs OEM | **PASS** — 237 ms medidos vs 235.5 OEM (tolerancia de cursores; configurado 235 ms) | `lockdrv_open_237mS.png` |
| `close` vs OEM | **PASS** — 235 ms | `lockdrv_close_235mS.png` |
| `cycle 4600` vs OEM auto-relock | **PASS** — 4.7 s medidos vs 4.58–4.82 s OEM | `lockdrv_cycle_dwell_4600mS.png` |
| Home pulse al boot | **PASS** — un pulso de retract tras reset, igual al power-up OEM | (misma forma que close) |
| `stress 20` | **PASS** — 20/20 ciclos, **20/20 retriggers rechazados**, 0 fallas; separación open→close 107 ms ≈ 235 + 100 guard + polling | `lockdrv_stress_dwell_100mS.png` |
| `abort` | **PASS (harness v1.2)** — ver nota de semántica abajo | log de consola |
| Idle < 1 µA | no re-medido; garantizado por diseño (coast → standby MX608E < 0.1 µA) | — |

### Semántica de `abort` (aclarada tras la primera corrida — harness v1.2)

* Es un **panic a nivel driver**: corta el pulso activo por hardware y fuerza
  coast. En v1.2 además **cancela la tarea de fondo** (`cycle`/`sweep`/`stress`).
* Abortar durante el dwell de un `cycle` cancela el close pendiente → el
  actuador puede quedar **extendido** (desenergizado, seguro). `close`/`home`
  lo devuelve a reposo.
* Un pulso de 235 ms es demasiado rápido para abortarlo tipeando; para probar
  el path de mid-pulse: `pulse ext 2000` y luego `abort` — el scope muestra el
  pulso cortado en el instante del abort.
* En v1.0 el usuario verificó que abort en idle es inofensivo y NO cancelaba
  la tarea de cycle (el close se ejecutaba igual) — v1.2 corrige ese acople.

### Estado de la etapa: VALIDADA EN BANCO — lista para integración en lock_app

Pendientes menores (no bloqueantes): re-medición a batería ~6 V, marcaje de
D8/D9, captura del delay de boot del OEM (para afinar `LOCKDRV_HOME_DELAY_MS`).

### Fix harness v1.3 (2026-08-16, panic en `abort` reportado por el usuario)

Bug introducido en v1.2 (mío): `cycle_task` se auto-eliminaba **sin limpiar
`s_bg_task`** → handle stale. Síntomas exactos del reporte: tras un `cycle`,
cualquier comando decía "background task already running", y `abort` llamaba
`vTaskDelete()` sobre un TCB ya liberado → `LoadProhibited` en `uxListRemove`
(EXCVADDR 0x4 = caminar una lista de un TCB muerto). Fix: (1) `cycle_task`
limpia el handle antes de `vTaskDelete(NULL)` (como ya hacían sweep/stress);
(2) `cmd_abort` endurecido: copia el handle, limpia el global PRIMERO y
recién después borra — un handle stale nunca se toca dos veces.

### Fix v1.3 validado en banco (2026-08-16) ✅ — ETAPA 5 CERRADA

Secuencia del usuario: cycle completo → stress 5 (5/5, retriggers rechazados)
→ abort en idle → open → abort → close. Sin panics, sin falsos positivos de
"tarea corriendo". El driver y el harness quedan aceptados para integración.
