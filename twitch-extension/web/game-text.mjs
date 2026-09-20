// Disciples II text directives, as emitted by interfaceutils.cpp / movepathhooks.cpp
// and present in TApp.dbf X005TA0423. Keep this grammar aligned with gametext.h.
// Rows contain plain text runs only. Callers render textContent and may apply the
// generated numeric RGB color; no HTML, font names, URLs or images are produced.
const digit = value => value >= '0' && value <= '9';
const letter = value => (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');

function unsignedField(source, begin, maxDigits, color = false) {
  let at = begin, value = 0;
  while (at < source.length && digit(source[at]) && at - begin < maxDigits) {
    value = value * 10 + Number(source[at++]);
  }
  if (at === begin || source[at] !== ';' || (color && value > 255)) return null;
  return {end: at + 1, value};
}

function directive(source, begin) {
  const command = source[begin + 1];
  let at = begin + 2;
  if (command === 'c' || command === 'o') {
    const components = [];
    for (let component = 0; component < 3; component++) {
      const field = unsignedField(source, at, 3, true);
      if (!field) return null;
      at = field.end; components.push(field.value);
    }
    return {end: at, command, components};
  }
  if (command === 'f') {
    if (!letter(source[at])) return null;
    while (at < source.length && (letter(source[at]) || digit(source[at]) || '_-'.includes(source[at]))) at++;
    return source[at] === ';' ? {end: at + 1, command} : null;
  }
  if (command === 'h' || command === 'v') {
    const values = command === 'h' ? ['L', 'C', 'R'] : ['T', 'C', 'B'];
    return values.includes(source[at]) && source[at + 1] === ';' ? {end: at + 2, command} : null;
  }
  if (command === 's' || command === 'p') {
    const field = unsignedField(source, at, 10);
    return field ? {...field, command} : null;
  }
  if (command === 'm' && source[at] === 'L') {
    const field = unsignedField(source, at + 1, 10);
    return field ? {...field, command} : null;
  }
  return null;
}

/**
 * @param {string|null|undefined} source Native marked-up text, already decoded to UTF-8.
 * @returns {Array<Array<{text: string, color?: string}>>} Rows and their adjacent text runs.
 */
export function parseGameText(source) {
  if (typeof source !== 'string' || !source.length) return [];
  const rows = [[]];
  let color;
  const current = () => rows[rows.length - 1];
  const lastText = () => current().at(-1)?.text ?? '';
  function append(text) {
    const previous = current().at(-1);
    if (previous && previous.color === color) previous.text += text;
    else current().push(color ? {text, color} : {text});
  }
  for (let at = 0; at < source.length;) {
    const value = source[at];
    if (value === '\n' || value === '\r') {
      rows.push([]);
      at += value === '\r' && source[at + 1] === '\n' ? 2 : 1;
      continue;
    }
    if (value !== '\\' || at + 1 >= source.length) { append(value); at++; continue; }
    const command = source[at + 1];
    if (command === 'n') { rows.push([]); at += 2; continue; }
    if (command === 't' || command === '\\') { append(command === 't' ? '\t' : '\\'); at += 2; continue; }
    const format = directive(source, at);
    if (!format) { append(value); at++; continue; }
    if (format.command === 'c') {
      color = format.components.every(component => component === 0)
        ? undefined : `rgb(${format.components.join(', ')})`;
    } else if (format.command === 'm') {
      // X005TA0423 resets left layout between the immunity and ward rows without \n.
      if (current().length) rows.push([]);
    } else if (format.command === 'p' && format.value > 0 && current().length && !/[ \t]$/.test(lastText())) {
      // A positive paragraph offset introduces the value column. Do not duplicate
      // the explicit \t already present in native templates. \p0 is only a reset.
      append('\t');
    }
    at = format.end;
  }
  return rows;
}
