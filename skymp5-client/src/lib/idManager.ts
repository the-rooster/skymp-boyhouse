export class IdManager {
  readonly version = 2;

  constructor(values: readonly (number | undefined)[] = []) {
    this.valueById = [...values];
    values.forEach((value, id) => {
      if (value === undefined) return;
      if (!Number.isInteger(value) || value < 0 || value > 0xffffffff || this.idByValue.has(value))
        throw Error('Invalid restored remote actor index');
      this.idByValue.set(value, id);
    });
    while (typeof this.valueById[this.minimumUnusedId] === 'number') ++this.minimumUnusedId;
  }

  allocateIdFor(value: number): number {
    if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) throw Error('Invalid remote actor index');
    const existing = this.idByValue.get(value);
    if (existing !== undefined) return existing;
    this.idByValue.set(value, this.minimumUnusedId);

    if (this.valueById.length <= this.minimumUnusedId) {
      this.valueById.length = this.minimumUnusedId + 1;
    }
    this.valueById[this.minimumUnusedId] = value;

    const res = this.minimumUnusedId;
    this.minimumUnusedId++;
    while (
      this.valueById.length > this.minimumUnusedId &&
      typeof this.valueById[this.minimumUnusedId] === "number"
    ) {
      this.minimumUnusedId++;
    }
    return res;
  }

  freeIdFor(value: number): void {
    const id = this.idByValue.get(value);
    if (id === undefined) return;
    if (id < this.minimumUnusedId) {
      this.minimumUnusedId = id;
    }
    this.idByValue.delete(value);
    this.valueById[id] = undefined;
    return;
  }

  getId(value: number): number {
    const r = this.idByValue.get(value);
    return typeof r === "number" ? r : -1;
  }

  getValueById(id: number): number | undefined {
    return this.valueById[id];
  }

  private idByValue = new Map<number, number>();
  private valueById = new Array<number | undefined>();
  private minimumUnusedId = 0;
}
