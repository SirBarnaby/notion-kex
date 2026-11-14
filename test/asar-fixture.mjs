import fs from 'node:fs';
import crypto from 'node:crypto';

export function writeAsar(file, members) {
  const header = { files: {} }, bodies = [];
  let offset = 0;
  for (const [name, source] of Object.entries(members)) {
    const data = Buffer.from(source), parts = name.split('/');
    let directory = header;
    for (const part of parts.slice(0, -1)) directory = directory.files[part] ||= { files: {} };
    const hash = crypto.createHash('sha256').update(data).digest('hex');
    directory.files[parts.at(-1)] = { size: data.length, offset: String(offset), integrity: { algorithm: 'SHA256', hash, blockSize: 4194304, blocks: [hash] } };
    bodies.push(data); offset += data.length;
  }
  const json = Buffer.from(JSON.stringify(header)), padded = Math.ceil(json.length / 4) * 4;
  const prefix = Buffer.alloc(16 + padded);
  [4, 8 + padded, 4 + padded, json.length].forEach((value, index) => prefix.writeUInt32LE(value, index * 4));
  json.copy(prefix, 16);
  fs.writeFileSync(file, Buffer.concat([prefix, ...bodies]));
}

export function readAsar(file) {
  const bytes = fs.readFileSync(file), header = JSON.parse(bytes.subarray(16, 16 + bytes.readUInt32LE(12)).toString());
  const body = 8 + bytes.readUInt32LE(4), members = {};
  const walk = (directory, prefix = '') => {
    for (const [name, entry] of Object.entries(directory.files)) {
      const memberPath = prefix + name;
      if (entry.files) walk(entry, memberPath + '/');
      else if (!entry.unpacked && !entry.link) {
        const data = bytes.subarray(body + Number(entry.offset), body + Number(entry.offset) + entry.size);
        if (entry.integrity && crypto.createHash('sha256').update(data).digest('hex') !== entry.integrity.hash) throw Error(`Integrity mismatch: ${memberPath}`);
        members[memberPath] = data;
      }
    }
  };
  walk(header); return members;
}
