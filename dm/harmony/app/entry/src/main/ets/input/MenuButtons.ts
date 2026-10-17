export class MenuButtons {
  private sources: Map<string, number> = new Map<string, number>();
  press(source: string, mask: number): void { this.sources.set(source, mask); }
  release(source: string): void { this.sources.delete(source); }
  reset(): void { this.sources.clear(); }
  value(): number {
    let value = 0;
    this.sources.forEach((mask: number) => { value |= mask; });
    return value;
  }
}
