export function utcOffsetMinutes(now = new Date()): number {
  return -now.getTimezoneOffset();
}

/** Handles ranges that wrap past midnight (e.g. 23:00 -> 08:00). */
export function isQuietHours(startMin: number, endMin: number, now = new Date()): boolean {
  const minutes = now.getHours() * 60 + now.getMinutes();
  if (startMin === endMin) return false;
  if (startMin < endMin) return minutes >= startMin && minutes < endMin;
  return minutes >= startMin || minutes < endMin;
}
