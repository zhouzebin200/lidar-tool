// Manual browser regression check: requires Node.js, Playwright, and Google Chrome.
// Run: node tests/browser/report_device_switch_check.js
const assert = require('node:assert/strict');
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

const root = path.join(__dirname, '..', '..', 'web');
const devices = [
  { id: 1, vendor: 'Hesai', model: 'A', ip: '192.168.1.11', source_port: 1, destination_port: 2368, packets: 10, reason: 'A' },
  { id: 2, vendor: 'RoboSense', model: 'B', ip: '192.168.1.22', source_port: 2, destination_port: 6699, packets: 20, reason: 'B' },
];
const fetched = [];
let delayA = false;
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname === '/api/devices') return json({ devices });
  if (url.pathname === '/api/status') return json({ version: 'test', native_plugins: { loaded: [] }, pcap: {}, capture: {} });
  if (url.pathname === '/api/lidar-web') {
    const ip = url.searchParams.get('ip');
    fetched.push(ip);
    const result = ip === devices[0].ip
      ? { success: true, data: { model: 'A', sn: 'SERIAL-A', brand: 'Hesai', hardware: 'HW-A', firmware: 'FW-A', ip }, section_notes: { 2: '[雷达网页端]\nA network' } }
      : { success: true, data: { model: 'B', sn: '', brand: 'RoboSense', hardware: '', firmware: '', ip }, section_notes: {} };
    if (ip === devices[0].ip && delayA) return setTimeout(() => json(result), 350);
    return json(result);
  }
  const file = path.join(root, url.pathname === '/' ? 'report.html' : url.pathname);
  if (!file.startsWith(root) || !fs.existsSync(file)) { res.writeHead(404); return res.end(); }
  res.setHeader('Content-Type', file.endsWith('.js') ? 'text/javascript' : file.endsWith('.css') ? 'text/css' : 'text/html');
  res.end(fs.readFileSync(file));
  function json(data) { res.setHeader('Content-Type', 'application/json'); res.end(JSON.stringify(data)); }
});

(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const browser = await chromium.launch({ headless: true, channel: 'chrome' });
  try {
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${server.address().port}/report.html`);
    await page.locator('#read').click();
    await page.locator('#devices').selectOption('0');
    await page.locator('#useDevice').click();
    await page.waitForFunction(() => document.querySelector('#meta input[data-meta="2"]').value === 'SERIAL-A');
    assert.equal(await page.locator('#assessmentInfo input[data-info="0"]').inputValue(), 'Hesai');
    await page.evaluate(() => { state.sections[2].notes += '\n\n手工记录'; });

    await page.locator('#devices').selectOption('1');
    await page.locator('#fetchLidarWeb').click();
    await page.waitForFunction(() => document.querySelector('#fetchLidarWeb').disabled === false);
    assert.equal(fetched.at(-1), devices[1].ip, 'one-click fetch must use newly selected LiDAR');
    assert.equal(await page.locator('#meta input[data-meta="2"]').inputValue(), '', 'old serial must be removed when new device has no serial');
    assert.equal(await page.locator('#assessmentInfo input[data-info="0"]').inputValue(), 'RoboSense');
    assert.equal(await page.locator('#assessmentInfo input[data-info="5"]').inputValue(), '', 'old hardware must be removed');
    assert.equal(await page.locator('#assessmentInfo input[data-info="7"]').inputValue(), devices[1].ip);
    assert.equal(await page.evaluate(() => state.sections[2].notes.includes('A network')), false, 'old imported network notes must be removed');
    assert.equal(await page.evaluate(() => state.sections[2].notes.includes('手工记录')), true, 'manual inspection notes must remain');

    delayA = true;
    await page.locator('#devices').selectOption('0');
    await page.locator('#useDevice').click();
    await page.locator('#devices').selectOption('1');
    await page.locator('#useDevice').click();
    await page.waitForTimeout(500);
    assert.equal(fetched.at(-1), devices[1].ip, 'switch during an in-flight request must fetch the new device');
    assert.equal(await page.locator('#assessmentInfo input[data-info="7"]').inputValue(), devices[1].ip, 'late response must not restore old device');

    const noteAfterSameIpSwitch = await page.evaluate(() => {
      devices[0].ip = devices[1].ip;
      document.querySelector('#devices').value = '0';
      document.querySelector('#devices').dispatchEvent(new Event('change'));
      document.querySelector('#useDevice').click();
      return state.sections[3].notes;
    });
    assert.equal(noteAfterSameIpSwitch.includes('查看器识别：RoboSense B'), false, 'changing physical device on the same IP must remove old automatic notes');
    console.log('report device switch: PASS');
  } finally {
    await browser.close();
    server.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; server.close(); });
