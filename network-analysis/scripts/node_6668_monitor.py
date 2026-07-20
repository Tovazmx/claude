#!/usr/bin/env python3
"""
Monitor de integridad para socket 6668/tcp - Linux embebido.
Segmento: 10.87.1.x | Nodo objetivo: 10.87.1.25
Uso defensivo: validacion de tramas y deteccion de anomalias.
"""

import socket
import time
import struct
import logging
import signal
import sys
import json
from datetime import datetime, timezone
from dataclasses import dataclass, field, asdict
from typing import Optional
from pathlib import Path


LOG_DIR = Path("/var/log/net_monitor")
TARGET_HOST = "10.87.1.25"
TARGET_PORT = 6668
CONNECT_TIMEOUT = 5.0
READ_TIMEOUT = 10.0
MONITOR_DURATION = 300  # 5 minutos por ciclo
BEACON_THRESHOLD = 0.05  # varianza normalizada para detectar beaconing
MAX_RECV_BUFFER = 4096


@dataclass
class SessionReport:
    timestamp: str = ""
    target: str = f"{TARGET_HOST}:{TARGET_PORT}"
    connect_time_ms: float = 0.0
    banner: str = ""
    banner_hex: str = ""
    frames_received: int = 0
    total_bytes: int = 0
    keepalive_detected: bool = False
    beaconing_detected: bool = False
    beacon_interval_s: float = 0.0
    recv_intervals: list = field(default_factory=list)
    anomalies: list = field(default_factory=list)
    irc_commands_seen: list = field(default_factory=list)
    duration_s: float = 0.0


IRC_COMMANDS = {
    b"PING", b"PONG", b"NICK", b"USER", b"JOIN", b"PART",
    b"PRIVMSG", b"NOTICE", b"QUIT", b"MODE", b"TOPIC",
    b"KICK", b"INVITE", b"001", b"002", b"003", b"004",
    b"005", b"372", b"375", b"376", b"ERROR",
}


logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    handlers=[
        logging.StreamHandler(sys.stdout),
    ],
)
log = logging.getLogger("net_monitor")

_shutdown = False


def _signal_handler(signum, frame):
    global _shutdown
    log.info("Senal recibida (%d), finalizando ciclo...", signum)
    _shutdown = True


signal.signal(signal.SIGINT, _signal_handler)
signal.signal(signal.SIGTERM, _signal_handler)


def validate_irc_frame(data: bytes) -> tuple[bool, list[str], list[str]]:
    """Valida si los datos recibidos son tramas IRC validas.
    Retorna (es_irc, comandos_encontrados, anomalias).
    """
    commands_found = []
    anomalies = []

    if not data:
        return False, commands_found, ["Trama vacia"]

    try:
        text = data.decode("utf-8", errors="replace")
    except Exception:
        anomalies.append("No decodificable como UTF-8")
        return False, commands_found, anomalies

    lines = text.split("\r\n")
    valid_lines = 0

    for line in lines:
        if not line:
            continue

        parts = line.lstrip(":").split()
        if not parts:
            continue

        if line.startswith(":") and len(parts) >= 2:
            cmd = parts[1].upper()
        else:
            cmd = parts[0].upper()

        cmd_bytes = cmd.encode("utf-8", errors="replace")
        if cmd_bytes in IRC_COMMANDS or cmd.isdigit():
            commands_found.append(cmd)
            valid_lines += 1
        else:
            anomalies.append(f"Comando no reconocido: {cmd}")

    is_irc = valid_lines > 0
    return is_irc, commands_found, anomalies


def detect_beaconing(intervals: list[float]) -> tuple[bool, float]:
    """Detecta patrones de beaconing por baja varianza en intervalos."""
    if len(intervals) < 3:
        return False, 0.0

    mean = sum(intervals) / len(intervals)
    if mean == 0:
        return False, 0.0

    variance = sum((x - mean) ** 2 for x in intervals) / len(intervals)
    normalized_variance = variance / (mean ** 2)

    return normalized_variance < BEACON_THRESHOLD, mean


def check_tcp_keepalive(sock: socket.socket) -> bool:
    """Verifica si el socket remoto tiene TCP keepalive habilitado."""
    try:
        val = sock.getsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE)
        return val != 0
    except OSError:
        return False


def measure_connection(host: str, port: int) -> tuple[Optional[socket.socket], float]:
    """Establece conexion TCP y mide el tiempo de establecimiento."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(CONNECT_TIMEOUT)

    t0 = time.monotonic()
    try:
        sock.connect((host, port))
        connect_ms = (time.monotonic() - t0) * 1000.0
        log.info("Conexion establecida en %.2f ms", connect_ms)
        return sock, connect_ms
    except (socket.timeout, ConnectionRefusedError, OSError) as e:
        elapsed = (time.monotonic() - t0) * 1000.0
        log.warning("Conexion fallida tras %.2f ms: %s", elapsed, e)
        sock.close()
        return None, elapsed


def capture_banner(sock: socket.socket, timeout: float = 3.0) -> bytes:
    """Captura el banner inicial del servicio."""
    sock.settimeout(timeout)
    try:
        data = sock.recv(MAX_RECV_BUFFER)
        return data
    except socket.timeout:
        return b""
    except OSError:
        return b""


def monitor_session(host: str, port: int, duration: float) -> SessionReport:
    """Ejecuta una sesion de monitoreo completa."""
    report = SessionReport(
        timestamp=datetime.now(timezone.utc).isoformat(),
    )

    sock, connect_ms = measure_connection(host, port)
    report.connect_time_ms = connect_ms

    if sock is None:
        report.anomalies.append("No se pudo establecer conexion TCP")
        return report

    try:
        report.keepalive_detected = check_tcp_keepalive(sock)

        banner = capture_banner(sock)
        if banner:
            report.banner = banner.decode("utf-8", errors="replace")[:512]
            report.banner_hex = banner[:128].hex()
            report.frames_received += 1
            report.total_bytes += len(banner)

            is_irc, cmds, anomalies = validate_irc_frame(banner)
            report.irc_commands_seen.extend(cmds)
            if not is_irc:
                report.anomalies.append(
                    f"Banner no es IRC valido: {anomalies}"
                )
            log.info("Banner capturado (%d bytes): %s", len(banner), report.banner[:80])
        else:
            log.info("Sin banner (timeout o servicio silencioso)")

        sock.settimeout(READ_TIMEOUT)
        start = time.monotonic()
        last_recv = start
        recv_intervals = []

        while (time.monotonic() - start) < duration and not _shutdown:
            try:
                data = sock.recv(MAX_RECV_BUFFER)
                if not data:
                    log.info("Conexion cerrada por el remoto")
                    break

                now = time.monotonic()
                interval = now - last_recv
                recv_intervals.append(interval)
                last_recv = now

                report.frames_received += 1
                report.total_bytes += len(data)

                is_irc, cmds, anomalies = validate_irc_frame(data)
                report.irc_commands_seen.extend(cmds)
                if anomalies:
                    report.anomalies.extend(anomalies)

                if len(data) == 0:
                    report.keepalive_detected = True

                log.debug("Recibido %d bytes (intervalo %.2fs)", len(data), interval)

            except socket.timeout:
                continue
            except OSError as e:
                report.anomalies.append(f"Error de socket: {e}")
                break

        report.duration_s = time.monotonic() - start
        report.recv_intervals = [round(i, 3) for i in recv_intervals[-50:]]

        if recv_intervals:
            is_beacon, mean_interval = detect_beaconing(recv_intervals)
            report.beaconing_detected = is_beacon
            report.beacon_interval_s = round(mean_interval, 3)
            if is_beacon:
                report.anomalies.append(
                    f"Patron de beaconing detectado (intervalo medio: {mean_interval:.2f}s)"
                )

        report.irc_commands_seen = list(set(report.irc_commands_seen))

    finally:
        sock.close()

    return report


def save_report(report: SessionReport):
    """Guarda el reporte en formato JSON."""
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    ts = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    path = LOG_DIR / f"session_{ts}.json"
    with open(path, "w") as f:
        json.dump(asdict(report), f, indent=2, default=str)
    log.info("Reporte guardado: %s", path)


def print_report(report: SessionReport):
    """Imprime resumen del reporte en consola."""
    print("\n" + "=" * 60)
    print(f"  REPORTE DE SESION - {report.target}")
    print("=" * 60)
    print(f"  Timestamp:          {report.timestamp}")
    print(f"  Conexion TCP:       {report.connect_time_ms:.2f} ms")
    print(f"  Duracion monitoreo: {report.duration_s:.1f} s")
    print(f"  Tramas recibidas:   {report.frames_received}")
    print(f"  Bytes totales:      {report.total_bytes}")
    print(f"  Keepalive TCP:      {'Si' if report.keepalive_detected else 'No'}")
    print(f"  Beaconing:          {'DETECTADO ({:.2f}s)'.format(report.beacon_interval_s) if report.beaconing_detected else 'No detectado'}")
    if report.banner:
        print(f"  Banner:             {report.banner[:60]}")
    if report.irc_commands_seen:
        print(f"  Comandos IRC:       {', '.join(report.irc_commands_seen)}")
    if report.anomalies:
        print(f"  Anomalias ({len(report.anomalies)}):")
        for a in report.anomalies[:10]:
            print(f"    - {a}")
    print("=" * 60 + "\n")


def main():
    log.info("Iniciando monitor de nodo %s:%d", TARGET_HOST, TARGET_PORT)
    log.info("Duracion por ciclo: %ds | Ctrl+C para detener", MONITOR_DURATION)

    cycle = 0
    while not _shutdown:
        cycle += 1
        log.info("--- Ciclo %d ---", cycle)

        report = monitor_session(TARGET_HOST, TARGET_PORT, MONITOR_DURATION)
        print_report(report)

        try:
            save_report(report)
        except PermissionError:
            log.warning("Sin permisos para escribir en %s, solo salida por consola", LOG_DIR)

        if not _shutdown:
            log.info("Esperando 10s antes del siguiente ciclo...")
            for _ in range(10):
                if _shutdown:
                    break
                time.sleep(1)

    log.info("Monitor finalizado.")


if __name__ == "__main__":
    main()
