'use strict';

const { Database } = require('./lib/index');
const path = require('path');

async function main() {
    const dbPath = path.join(__dirname, 'example_db');
    const db = new Database(dbPath);

    console.log('Opening database...');
    await db.open();

    console.log('Creating nodes...');
    const alice = await db.createNode('Person', { name: 'Alice', age: 30 });
    const bob = await db.createNode('Person', { name: 'Bob', age: 25 });
    const charlie = await db.createNode('Person', { name: 'Charlie', age: 35 });

    console.log(`Created nodes: ${alice}, ${bob}, ${charlie}`);

    console.log('Creating relationships...');
    await db.createEdge(alice, bob, 'KNOWS', { since: 2020 });
    await db.createEdge(bob, charlie, 'KNOWS', { since: 2021 });

    console.log('Querying with Cypher...');
    const result = await db.execute(`
        MATCH (a:Person)-[r:KNOWS]->(b:Person)
        RETURN a.name AS from, b.name AS to, r.since AS since
        ORDER BY r.since
    `);

    console.log(`\nQuery results (${result.duration.toFixed(2)}ms):`);
    console.log('Columns:', result.columns);
    for (const row of result) {
        console.log(`  ${row.from} knows ${row.to} since ${row.since}`);
    }

    console.log('\nUsing parameterized query...');
    const result2 = await db.execute(
        'MATCH (n:Person {name: $name}) RETURN n.name AS name, n.age AS age',
        { name: 'Alice' }
    );
    const person = result2.single();
    console.log(`  Found: ${person.name}, age ${person.age}`);

    console.log('\nFinding shortest path...');
    const path = await db.getShortestPath(alice, charlie);
    console.log(`  Path length: ${path.length}`);
    console.log(`  Path: ${path.map(n => n.properties.name).join(' -> ')}`);

    console.log('\nUsing transaction...');
    const tx = await db.beginTransaction();
    try {
        await tx.execute('CREATE (n:Person {name: $name})', { name: 'David' });
        await tx.execute('CREATE (n:Person {name: $name})', { name: 'Eve' });
        await tx.commit();
        console.log('  Transaction committed');
    } catch (e) {
        await tx.rollback();
        console.error('  Transaction rolled back:', e);
    }

    console.log('\nCounting all nodes...');
    const countResult = await db.execute('MATCH (n) RETURN count(n) AS count');
    console.log(`  Total nodes: ${countResult.scalar()}`);

    console.log('\nClosing database...');
    await db.close();
    console.log('Done!');
}

main().catch(console.error);
