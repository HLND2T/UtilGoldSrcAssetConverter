// Validates .glb/.gltf files with the Khronos glTF-Validator (npm package gltf-validator, resolved through
// NODE_PATH). Prints one summary line per file and every error; exits with 1 when any file has errors.
'use strict';

const fs = require('fs');
const path = require('path');
const validator = require('gltf-validator');

async function validate(file) {
    const bytes = new Uint8Array(fs.readFileSync(file));
    const report = await validator.validateBytes(bytes, {
        uri: path.basename(file),
        maxIssues: 0,
        externalResourceFunction: (uri) =>
            new Promise((resolve, reject) => {
                fs.readFile(path.resolve(path.dirname(file), decodeURIComponent(uri)), (error, data) =>
                    error ? reject(error.toString()) : resolve(new Uint8Array(data)));
            }),
    });

    const issues = report.issues;
    console.log(`${file}: ${issues.numErrors} errors, ${issues.numWarnings} warnings, ${issues.numInfos} infos, ${issues.numHints} hints`);

    for (const message of issues.messages) {
        if (message.severity <= 1)
            console.log(`  [${message.severity === 0 ? 'error' : 'warning'}] ${message.code} ${message.pointer || ''}: ${message.message}`);
    }

    return issues.numErrors;
}

(async () => {
    let errors = 0;

    for (const file of process.argv.slice(2))
        errors += await validate(file);

    process.exit(errors ? 1 : 0);
})().catch((error) => {
    console.error(error);
    process.exit(1);
});
