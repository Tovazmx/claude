# Analisis de Red - Nodo 10.87.1.25 / Puerto 6668/tcp

## Resumen Ejecutivo

| Campo | Valor |
|---|---|
| Segmento | 10.87.1.x /24 |
| Nodo objetivo | 10.87.1.25 |
| Puerto | 6668/tcp |
| Protocolo preliminar | IRC-like / canal de control |
| Latencia promedio | 3.94 ms |
| IP externa en vecindad | 210.107.57.202 |

## 1. Evaluacion del Socket 6668/tcp

### 1.1 Identificacion del servicio

El puerto 6668/tcp es un puerto alternativo comun para:

- **IRC (Internet Relay Chat)**: Los puertos 6660-6669 son rangos estandar para servidores IRC. El 6667 es el predeterminado, 6668 es el primer alternativo.
- **C2 (Command & Control)**: Malware historicamente ha usado puertos IRC para canales de control (botnets clasicas como Eggdrop, mIRC-based, o variantes modernas que emulan el protocolo).
- **Servicios personalizados**: Aplicaciones que usan sockets TCP en puertos no privilegiados para telemetria o monitoreo.

### 1.2 Indicadores a verificar

| Indicador | Que buscar | Riesgo |
|---|---|---|
| Banner del servicio | Respuesta inicial al conectar (NOTICE, PING, cabeceras HTTP) | Identifica protocolo real |
| Persistencia de conexion | El socket mantiene sesion sin enviar datos (keepalive) | Tipico de C2 |
| Volumen de trafico | Bytes tx/rx en idle vs activo | Exfiltracion si hay trafico constante en idle |
| Destinos de conexion | Conexiones salientes desde el nodo hacia IPs externas | Lateral movement o beaconing |
| Patrones temporales | Intervalos regulares de trafico (beaconing) | C2 con heartbeat |

### 1.3 Verificacion de persistencia

Para evaluar si existe persistencia de conexion:

```bash
# Verificar estado del socket desde otro nodo en el segmento
ss -tnp | grep 6668
netstat -tnp | grep 6668

# Captura pasiva de trafico (requiere privilegios)
tcpdump -i eth0 host 10.87.1.25 and port 6668 -w capture_6668.pcap -c 1000

# Verificar conexiones establecidas hacia el exterior
conntrack -L | grep 10.87.1.25
```

## 2. IP Externa 210.107.57.202

### 2.1 Contexto

La presencia de una IP externa en la tabla de vecindad (ARP/NDP) de un segmento local es anomala. Las tablas ARP solo registran direcciones del mismo dominio de broadcast L2. Posibles explicaciones:

- **Proxy ARP habilitado** en el gateway, respondiendo con su MAC para IPs externas.
- **Tunel o VPN** terminando en un nodo local que expone la IP en la tabla.
- **Misconfiguracion de red** donde la IP fue asignada localmente en un rango incorrecto.
- **Actividad maliciosa**: ARP spoofing para redirigir trafico hacia un nodo comprometido.

### 2.2 Reconocimiento pasivo recomendado

```bash
# Verificar entrada ARP
arp -a | grep 210.107
ip neigh show | grep 210.107

# Verificar ruta
ip route get 210.107.57.202

# Verificar si el gateway responde por esa IP (Proxy ARP)
arping -I eth0 210.107.57.202
```

## 3. Arquitectura de Validacion Propuesta

```
[Nodo Monitor ESP32-S3/Linux]
        |
        | Socket TCP -> 10.87.1.25:6668
        |
        v
+-------------------+
| Conectar al nodo  |
| Capturar banner   |
| Medir keepalive   |
| Analizar tramas   |
| Registrar anomal. |
+-------------------+
        |
        v
[Log local / Serial / Syslog]
```

### 3.1 Metricas que capturan los scripts

- Tiempo de establecimiento de conexion TCP (SYN -> SYN-ACK)
- Banner o respuesta inicial del servicio
- Deteccion de keepalive TCP (paquetes sin payload)
- Intervalos entre recepciones (deteccion de beaconing)
- Integridad basica de tramas (validacion de formato IRC si aplica)
- Volumen de datos recibidos en ventanas de tiempo

## 4. Entregables

| Archivo | Descripcion |
|---|---|
| `scripts/node_6668_monitor.py` | Monitor Python para Linux embebido |
| `scripts/node_6668_esp32.cpp` | Sketch C++ para ESP32-S3 (Arduino framework) |
