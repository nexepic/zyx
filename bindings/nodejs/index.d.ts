export interface Node {
    id: number;
    label: string;
    labels: string[];
    properties: Record<string, any>;
}

export interface Edge {
    id: number;
    sourceId: number;
    targetId: number;
    type: string;
    properties: Record<string, any>;
}

export class Record {
    constructor(data: Record<string, any>, columns: string[]);

    get(key: string | number): any;
    keys(): string[];
    values(): any[];
    data(): Record<string, any>;
    readonly length: number;

    // Allow property access
    [key: string]: any;
}

export class Result {
    constructor(nativeResult: any);

    readonly columns: string[];
    readonly isSuccess: boolean;
    readonly error: string | null;
    readonly duration: number;
    readonly records: Record[];

    [Symbol.iterator](): Iterator<Record>;
    fetchAll(): Record[];
    single(strict?: boolean): Record;
    scalar(): any;
    data(): Array<Record<string, any>>;
}

export class Transaction {
    constructor(nativeTx: any);

    execute(cypher: string, params?: Record<string, any>): Promise<Result>;
    commit(): Promise<void>;
    rollback(): Promise<void>;

    readonly isActive: boolean;
    readonly isReadOnly: boolean;
}

export class Database {
    constructor(dbPath: string);

    open(): Promise<void>;
    close(): Promise<void>;
    save(): Promise<void>;

    execute(cypher: string, params?: Record<string, any>): Promise<Result>;

    beginTransaction(): Promise<Transaction>;
    beginReadOnlyTransaction(): Promise<Transaction>;

    readonly hasActiveTransaction: boolean;

    createNode(label: string | string[], props?: Record<string, any>): Promise<number>;
    createNodes(label: string, propsList: Array<Record<string, any>>): Promise<number[]>;

    createEdge(
        srcId: number,
        dstId: number,
        edgeType: string,
        props?: Record<string, any>
    ): Promise<number>;

    createEdges(
        edgeType: string,
        edges: Array<[number, number, Record<string, any>?]>
    ): Promise<number[]>;

    getShortestPath(startId: number, endId: number, maxDepth?: number): Promise<Node[]>;
}

export { Database as default };
