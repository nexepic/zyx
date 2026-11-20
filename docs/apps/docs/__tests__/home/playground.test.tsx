import React from 'react'
import { renderToString } from 'react-dom/server'
import { fireEvent, render, screen } from '@testing-library/react'
import { CypherPlayground, executeGdsPreparationQueries } from '../../home/playground'
import { GdsPanel } from '../../home/gds-panel'

jest.mock('next/link', () => {
  return ({ children, href, ...props }: any) => (
    <a href={href} {...props}>
      {children}
    </a>
  )
})

jest.mock('../../home/graph-view', () => ({
  GraphView: () => <div data-testid="graph-view" />,
}))

describe('Cypher playground UI regressions', () => {
  const originalNavigator = global.navigator

  afterEach(() => {
    Object.defineProperty(global, 'navigator', {
      configurable: true,
      value: originalNavigator,
    })
  })

  it('renders the keyboard shortcut consistently before hydration', () => {
    Object.defineProperty(global, 'navigator', {
      configurable: true,
      value: undefined,
    })
    const serverHtml = renderToString(<CypherPlayground isEn homeLink="/" />)

    Object.defineProperty(global, 'navigator', {
      configurable: true,
      value: { platform: 'MacIntel' },
    })
    const clientHtml = renderToString(<CypherPlayground isEn homeLink="/" />)

    expect(clientHtml).toBe(serverHtml)
    expect(clientHtml).toContain('Ctrl')
    expect(clientHtml).toContain('Enter')
  })

  const plan = {
    algorithm: 'pageRank',
    exists: "CALL gds.graph.exists('__pg')",
    drop: "CALL gds.graph.drop('__pg')",
    project: "CALL gds.graph.project('__pg', '', '')",
    run: "CALL gds.pageRank.stream('__pg') YIELD nodeId, score RETURN nodeId, score ORDER BY score DESC",
  } as const

  function renderAnalytics() {
    const onRunGds = jest.fn()
    render(
      <GdsPanel
        isEn
        schema={{ nodes: [{ label: 'Person', props: ['name'] }], edges: [{ type: 'KNOWS' }] }}
        onRunGds={onRunGds}
        status="ready"
      />,
    )
    return onRunGds
  }

  it('builds an explicit projection plan before running an algorithm', () => {
    const onRunGds = renderAnalytics()
    fireEvent.click(screen.getByRole('button', { name: 'Person' }))
    fireEvent.click(screen.getByRole('button', { name: 'KNOWS' }))
    fireEvent.click(screen.getByRole('button', { name: 'Run PageRank' }))

    expect(onRunGds).toHaveBeenCalledTimes(1)
    expect(onRunGds).toHaveBeenCalledWith({
      ...plan,
      project: "CALL gds.graph.project('__pg', 'Person', 'KNOWS')",
    }, { nodeLabel: 'Person', edgeType: 'KNOWS' })
  })

  it('uses default PageRank iterations when specifying damping only', () => {
    const onRunGds = renderAnalytics()
    fireEvent.change(screen.getByPlaceholderText('0.85'), { target: { value: ' 0.7 ' } })
    fireEvent.click(screen.getByRole('button', { name: 'Run PageRank' }))
    expect(onRunGds).toHaveBeenCalledWith({
      ...plan,
      run: "CALL gds.pageRank.stream('__pg', 20, 0.7) YIELD nodeId, score RETURN nodeId, score ORDER BY score DESC",
    }, { nodeLabel: '', edgeType: '' })
  })

  it.each([
    ['Connected Components', 'wcc', "CALL gds.wcc.stream('__pg') YIELD nodeId, componentId RETURN nodeId, componentId ORDER BY componentId, nodeId"],
    ['Betweenness Centrality', 'betweenness', "CALL gds.betweenness.stream('__pg') YIELD nodeId, score RETURN nodeId, score ORDER BY score DESC"],
    ['Closeness Centrality', 'closeness', "CALL gds.closeness.stream('__pg') YIELD nodeId, score RETURN nodeId, score ORDER BY score DESC"],
  ])('builds a supported query for %s', (name, algorithm, run) => {
    const onRunGds = renderAnalytics()
    fireEvent.click(screen.getByRole('button', { name }))
    fireEvent.click(screen.getByRole('button', { name: `Run ${name}` }))
    expect(onRunGds).toHaveBeenCalledWith({ ...plan, algorithm, run }, { nodeLabel: '', edgeType: '' })
  })

  it('validates shortest path IDs before building a query', () => {
    const onRunGds = renderAnalytics()
    fireEvent.click(screen.getByRole('button', { name: 'Shortest Path' }))
    const start = screen.getByPlaceholderText('1')
    const end = screen.getByPlaceholderText('10')
    const runButton = screen.getByRole('button', { name: 'Run Shortest Path' })
    fireEvent.change(start, { target: { value: '1.5' } })
    fireEvent.change(end, { target: { value: '10' } })
    fireEvent.click(runButton)
    expect(screen.getByRole('alert')).toHaveTextContent('Start node ID must be an integer')
    expect(onRunGds).not.toHaveBeenCalled()

    fireEvent.change(start, { target: { value: ' 001 ' } })
    fireEvent.click(runButton)
    expect(screen.queryByRole('alert')).not.toBeInTheDocument()
    expect(onRunGds).toHaveBeenCalledWith({
      ...plan,
      algorithm: 'dijkstra',
      run: "CALL gds.shortestPath.dijkstra.stream('__pg', 1, 10) YIELD nodeId, cost RETURN nodeId, cost",
    }, { nodeLabel: '', edgeType: '' })
  })

  it.each([
    [false, [plan.exists, plan.project]],
    [true, [plan.exists, plan.drop, plan.project]],
  ])('drops the old projection only when the engine reports it exists (%s)', (exists, queries) => {
    const executeReadOnly = jest.fn()
      .mockReturnValueOnce({ columns: ['graphName', 'exists'], rows: [['__pg', exists]] })
    executeGdsPreparationQueries({ executeReadOnly }, 123, plan)
    expect(executeReadOnly.mock.calls).toEqual(queries.map((query) => [123, query]))
  })

  it('does not swallow a real drop failure even if its text contains “not found”', () => {
    const error = new Error('Required projection authentication metadata not found')
    const executeReadOnly = jest.fn()
      .mockReturnValueOnce({ columns: ['exists'], rows: [[true]] })
      .mockImplementationOnce(() => { throw error })
    expect(() => executeGdsPreparationQueries({ executeReadOnly }, 123, plan)).toThrow(error)
    expect(executeReadOnly.mock.calls).toEqual([[123, plan.exists], [123, plan.drop]])
  })

  it('rejects an invalid exists result instead of guessing from its string form', () => {
    const executeReadOnly = jest.fn()
      .mockReturnValueOnce({ columns: ['exists'], rows: [['false']] })
    expect(() => executeGdsPreparationQueries({ executeReadOnly }, 123, plan)).toThrow('Unexpected graph existence result')
    expect(executeReadOnly).toHaveBeenCalledTimes(1)
  })
})
