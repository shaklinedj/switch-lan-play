import * as dgram from 'dgram';
import * as net from 'net';

// Configuración
const RELAY_HOST = 'tekn0.net';
const RELAY_PORT = 11451;
const VIRTUAL_IP = '0.0.0.0'; // Bind a todas las interfaces
const BROADCAST_IP = '10.13.255.255';

// CommId de Mario Kart 8 Deluxe (little-endian)
const MK8DX_COMM_ID = Buffer.from([0x00, 0x02, 0x20, 0x00, 0x00, 0x15, 0x20, 0x01]);

// Función para crear paquete ScanResp
function createScanResp(): Buffer {
    const header = Buffer.alloc(12);
    header.writeUInt16LE(0x0001, 0); // type = ScanResp
    header.writeUInt16LE(0x0000, 2); // flags
    header.writeUInt32LE(0x00000000, 4); // seq
    header.writeUInt16LE(240, 8); // bodyLen
    header.writeUInt16LE(0x0000, 10); // padding

    const body = Buffer.alloc(240);

    // CommId (offset 0)
    MK8DX_COMM_ID.copy(body, 0);

    // SceneId (offset 8)
    body.writeUInt32LE(0, 8);

    // Usuario (offset 116, 32 bytes)
    const username = 'Sala Falsa';
    const usernameBuf = Buffer.from(username, 'utf8');
    usernameBuf.copy(body, 116, 0, Math.min(usernameBuf.length, 32));

    // Rellenar con datos dummy
    for (let i = 0; i < 240; i++) {
        if (body[i] === 0) body[i] = 0x41; // 'A'
    }

    return Buffer.concat([header, body]);
}

// Conectar al relay
const socket = dgram.createSocket('udp4');

console.log('Sala simulada corriendo, conectando al relay...');

// Enviar heartbeat al relay
setInterval(() => {
    const heartbeat = Buffer.from(`SLP-PC-PEER:10.13.1.102`);
    socket.send(heartbeat, 0, heartbeat.length, RELAY_PORT, RELAY_HOST);
}, 5000);

// Enviar Scan periódicamente para mantenerse activo
setInterval(() => {
    const scan = Buffer.alloc(12);
    scan.writeUInt16LE(0x0000, 0); // type = Scan
    scan.writeUInt16LE(0x0000, 2); // flags
    scan.writeUInt32LE(0x00000000, 4); // seq
    scan.writeUInt16LE(0, 8); // bodyLen
    scan.writeUInt16LE(0x0000, 10); // padding
    socket.send(scan, 0, scan.length, RELAY_PORT, RELAY_HOST);
}, 1000);

// Escuchar mensajes del relay
socket.on('message', (msg, rinfo) => {
    console.log(`Mensaje recibido de ${rinfo.address}:${rinfo.port}, tamaño: ${msg.length}`);
    if (msg.length >= 12) {
        const type = msg.readUInt16LE(0);
        if (type === 0x0000) { // Scan
            console.log('Recibido Scan, enviando ScanResp');
            // Enviar ScanResp
            const scanResp = createScanResp();
            socket.send(scanResp, 0, scanResp.length, RELAY_PORT, RELAY_HOST);
        }
    }
});

socket.on('message', (msg, rinfo) => {
    // Verificar si es un paquete Scan
    if (msg.length >= 12 && msg.readUInt16LE(0) === 0x0000) { // type = Scan
        console.log(`Recibido Scan desde ${rinfo.address}:${rinfo.port}`);

        // Responder con ScanResp
        const scanResp = createScanResp();
        socket.send(scanResp, 0, scanResp.length, RELAY_PORT, RELAY_HOST);
        console.log('Enviado ScanResp simulando sala de Mario Kart 8 Deluxe');
    }
});

socket.on('error', (err) => {
    console.error('Error:', err);
});