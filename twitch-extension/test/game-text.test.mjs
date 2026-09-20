import test from 'node:test';
import assert from 'node:assert/strict';
import {parseGameText} from '../web/game-text.mjs';

const plain = rows => rows.map(row => row.map(run => run.text).join('')).join('\n');

test('real modified-number template keeps only actual bonus green, with black resetting to card color', () => {
  assert.deepEqual(parseGameText(String.raw`Защита:\t40 \c025;090;000;+ 15\c000;000;000; (55)`), [[
    {text: 'Защита:\t40 '}, {text: '+ 15', color: 'rgb(25, 90, 0)'}, {text: ' (55)'}
  ]]);
});

test('negative bonus stays red and outline color never replaces text color', () => {
  assert.deepEqual(parseGameText(String.raw`\c100;000;000;-25%\o000;000;000; урон\c000;000;000;`), [[
    {text: '-25% урон', color: 'rgb(100, 0, 0)'}
  ]]);
  assert.deepEqual(parseGameText(String.raw`\fmedium;\hC;\vT;\c255;255;255;\o000;000;000;27`), [[
    {text: '27', color: 'rgb(255, 255, 255)'}
  ]]);
});

test('native immunity and ward layout becomes distinct rows with tabs retained', () => {
  const input = String.raw`\s97;\fMedbold;Иммунитет:\t\fNormal;\p97;Нет\mL0;\fMedbold;Стойкость:\t\fNormal;\p97;Огонь`;
  assert.deepEqual(parseGameText(input), [[{text: 'Иммунитет:\tНет'}], [{text: 'Стойкость:\tОгонь'}]]);
});

test('paragraph offset separates a value only when a separator is absent', () => {
  assert.deepEqual(parseGameText(String.raw`Тип:\p99;Оружие\p0;\nТочность:\t\p99;80%`), [
    [{text: 'Тип:\tОружие'}], [{text: 'Точность:\t80%'}]
  ]);
  assert.equal(plain(parseGameText(String.raw`Тип: \p99;Оружие`)), 'Тип: Оружие');
  assert.equal(plain(parseGameText(String.raw`\p97;Лидерство: 3`)), 'Лидерство: 3');
});

test('font, alignment and tab-stop directives do not leak code fragments or insert text', () => {
  assert.deepEqual(parseGameText(String.raw`\hL;\vC;\fMedBold;Урон:\fNormal; \s60;\t75`), [[{text: 'Урон: \t75'}]]);
  assert.equal(plain(parseGameText(String.raw`\mL3;Уровень: 5\n\mL0;Опыт: 100/200`)), 'Уровень: 5\nОпыт: 100/200');
});

test('RGB is consumed as three complete numeric fields, never as a generic semicolon cleanup', () => {
  assert.equal(plain(parseGameText(String.raw`000;000; 090;000; \c025;090;000;15\c000;000;000; 100/100`)), '000;000; 090;000; 15 100/100');
  const directive = String.raw`\c025;090;000;`;
  for (let length = 0; length < directive.length; length++) {
    assert.equal(plain(parseGameText(directive.slice(0, length))), directive.slice(0, length));
  }
  assert.equal(plain(parseGameText(directive)), '');
  for (const malformed of [String.raw`\c256;000;000;40`, String.raw`\c025;word;000;37`, String.raw`\o000;000;`]) {
    assert.equal(plain(parseGameText(malformed)), malformed);
  }
});

test('unknown or malformed directives remain text, including their numbers and punctuation', () => {
  for (const value of [String.raw`\qKeep 15;090;000; End`, String.raw`\fNormal Text; 75`, String.raw`\mQ0;Word`, String.raw`\p-2;42`, 'Text\\']) {
    assert.equal(plain(parseGameText(value)), value);
  }
});

test('existing whitespace, multiple newlines, escaped backslashes and Unicode survive', () => {
  assert.equal(plain(parseGameText('  A\r\n\n\nB\rC\t  ')), '  A\n\n\nB\nC\t  ');
  assert.equal(plain(parseGameText(String.raw`A\\n B\\c025;090;000;`)), String.raw`A\n B\c025;090;000;`);
  assert.equal(plain(parseGameText(String.raw`👑 Жрец\n+15 ОЗ, −10% опыта`)), '👑 Жрец\n+15 ОЗ, −10% опыта');
  assert.deepEqual(parseGameText(null), []);
  assert.deepEqual(parseGameText(undefined), []);
  assert.deepEqual(parseGameText(''), []);
});

test('color persists across lines until an explicit reset and adjacent same-color runs coalesce', () => {
  assert.deepEqual(parseGameText(String.raw`\c025;090;000;+15\n\fNormal;+20\c025;090;000;%\c000;000;000; HP`), [
    [{text: '+15', color: 'rgb(25, 90, 0)'}],
    [{text: '+20%', color: 'rgb(25, 90, 0)'}, {text: ' HP'}]
  ]);
});

test('HTML-like strings remain inert text and only validated numeric RGB can enter style data', () => {
  const input = String.raw`<img src=x onerror=alert(1)>\c025;090;000;<script>alert(2)</script>\c000;000;000;\cred;url(javascript:evil);0;`;
  const rows = parseGameText(input);
  assert.deepEqual(rows, [[
    {text: '<img src=x onerror=alert(1)>'},
    {text: '<script>alert(2)</script>', color: 'rgb(25, 90, 0)'},
    {text: String.raw`\cred;url(javascript:evil);0;`}
  ]]);
  for (const run of rows.flat()) {
    assert.deepEqual(Object.keys(run).sort(), run.color ? ['color', 'text'] : ['text']);
    if (run.color) assert.match(run.color, /^rgb\(\d{1,3}, \d{1,3}, \d{1,3}\)$/);
  }
});
