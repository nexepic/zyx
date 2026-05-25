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

  it('builds a projection before running an algorithm', () => {
    const onRunGds = jest.fn()

    render(
      <GdsPanel
        isEn
        schema={{ nodes: [{ label: 'Person', props: ['name'] }], edges: [{ type: 'KNOWS' }] }}
        onRunGds={onRunGds}
        status="ready"
      />,
    )

    fireEvent.click(screen.getByRole('button', { name: 'Run PageRank' }))

    expect(onRunGds).toHaveBeenCalledTimes(1)
    const [queries, scope] = onRunGds.mock.calls[0]
    expect(queries).toEqual([
      "CALL gds.graph.drop('__pg')",
      "CALL gds.graph.project('__pg', '', '')",
      "CALL gds.pageRank.stream('__pg') YIELD nodeId, score RETURN nodeId, score ORDER BY score DESC",
    ])
    expect(scope).toEqual({ nodeLabel: '', edgeType: '' })
  })

  it('continues GDS preparation when the existing projection is absent', () => {
    const driver = {
      executeReadOnly: jest.fn((_: number, query: string) => {
        if (query === "CALL gds.graph.drop('__pg')") {
          throw new Error("Graph projection '__pg' not found")
        }
        return { columns: [], rows: [] }
      }),
    }

    executeGdsPreparationQueries(driver as any, 123, [
      "CALL gds.graph.drop('__pg')",
      "CALL gds.graph.project('__pg', '', '')",
      "CALL gds.pageRank.stream('__pg') YIELD nodeId, score RETURN nodeId, score ORDER BY score DESC",
    ])

    expect(driver.executeReadOnly).toHaveBeenCalledTimes(2)
    expect(driver.executeReadOnly).toHaveBeenNthCalledWith(1, 123, "CALL gds.graph.drop('__pg')")
    expect(driver.executeReadOnly).toHaveBeenNthCalledWith(2, 123, "CALL gds.graph.project('__pg', '', '')")
  })
})
