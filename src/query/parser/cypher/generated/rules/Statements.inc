//----------------- CypherContext ------------------------------------------------------------------

CypherParser::CypherContext::CypherContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::StatementContext* CypherParser::CypherContext::statement() {
  return getRuleContext<CypherParser::StatementContext>(0);
}

tree::TerminalNode* CypherParser::CypherContext::EOF() {
  return getToken(CypherParser::EOF, 0);
}

tree::TerminalNode* CypherParser::CypherContext::SEMI() {
  return getToken(CypherParser::SEMI, 0);
}


size_t CypherParser::CypherContext::getRuleIndex() const {
  return CypherParser::RuleCypher;
}


std::any CypherParser::CypherContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCypher(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::CypherContext* CypherParser::cypher() {
  CypherContext *_localctx = _tracker.createInstance<CypherContext>(_ctx, getState());
  enterRule(_localctx, 0, CypherParser::RuleCypher);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(198);
    statement();
    setState(200);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::SEMI) {
      setState(199);
      match(CypherParser::SEMI);
    }
    setState(202);
    match(CypherParser::EOF);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- StatementContext ------------------------------------------------------------------

CypherParser::StatementContext::StatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}


size_t CypherParser::StatementContext::getRuleIndex() const {
  return CypherParser::RuleStatement;
}

void CypherParser::StatementContext::copyFrom(StatementContext *ctx) {
  ParserRuleContext::copyFrom(ctx);
}

//----------------- ProfileStatementContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::ProfileStatementContext::K_PROFILE() {
  return getToken(CypherParser::K_PROFILE, 0);
}

CypherParser::QueryContext* CypherParser::ProfileStatementContext::query() {
  return getRuleContext<CypherParser::QueryContext>(0);
}

CypherParser::AdministrationStatementContext* CypherParser::ProfileStatementContext::administrationStatement() {
  return getRuleContext<CypherParser::AdministrationStatementContext>(0);
}

CypherParser::ProfileStatementContext::ProfileStatementContext(StatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::ProfileStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProfileStatement(this);
  else
    return visitor->visitChildren(this);
}
//----------------- ExplainStatementContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::ExplainStatementContext::K_EXPLAIN() {
  return getToken(CypherParser::K_EXPLAIN, 0);
}

CypherParser::QueryContext* CypherParser::ExplainStatementContext::query() {
  return getRuleContext<CypherParser::QueryContext>(0);
}

CypherParser::AdministrationStatementContext* CypherParser::ExplainStatementContext::administrationStatement() {
  return getRuleContext<CypherParser::AdministrationStatementContext>(0);
}

CypherParser::ExplainStatementContext::ExplainStatementContext(StatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::ExplainStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitExplainStatement(this);
  else
    return visitor->visitChildren(this);
}
//----------------- AdminStatementContext ------------------------------------------------------------------

CypherParser::AdministrationStatementContext* CypherParser::AdminStatementContext::administrationStatement() {
  return getRuleContext<CypherParser::AdministrationStatementContext>(0);
}

CypherParser::AdminStatementContext::AdminStatementContext(StatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::AdminStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitAdminStatement(this);
  else
    return visitor->visitChildren(this);
}
//----------------- RegularStatementContext ------------------------------------------------------------------

CypherParser::QueryContext* CypherParser::RegularStatementContext::query() {
  return getRuleContext<CypherParser::QueryContext>(0);
}

CypherParser::RegularStatementContext::RegularStatementContext(StatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::RegularStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRegularStatement(this);
  else
    return visitor->visitChildren(this);
}
CypherParser::StatementContext* CypherParser::statement() {
  StatementContext *_localctx = _tracker.createInstance<StatementContext>(_ctx, getState());
  enterRule(_localctx, 2, CypherParser::RuleStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(216);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 3, _ctx)) {
    case 1: {
      _localctx = _tracker.createInstance<CypherParser::ExplainStatementContext>(_localctx);
      enterOuterAlt(_localctx, 1);
      setState(204);
      match(CypherParser::K_EXPLAIN);
      setState(207);
      _errHandler->sync(this);
      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 1, _ctx)) {
      case 1: {
        setState(205);
        query();
        break;
      }

      case 2: {
        setState(206);
        administrationStatement();
        break;
      }

      default:
        break;
      }
      break;
    }

    case 2: {
      _localctx = _tracker.createInstance<CypherParser::ProfileStatementContext>(_localctx);
      enterOuterAlt(_localctx, 2);
      setState(209);
      match(CypherParser::K_PROFILE);
      setState(212);
      _errHandler->sync(this);
      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 2, _ctx)) {
      case 1: {
        setState(210);
        query();
        break;
      }

      case 2: {
        setState(211);
        administrationStatement();
        break;
      }

      default:
        break;
      }
      break;
    }

    case 3: {
      _localctx = _tracker.createInstance<CypherParser::RegularStatementContext>(_localctx);
      enterOuterAlt(_localctx, 3);
      setState(214);
      query();
      break;
    }

    case 4: {
      _localctx = _tracker.createInstance<CypherParser::AdminStatementContext>(_localctx);
      enterOuterAlt(_localctx, 4);
      setState(215);
      administrationStatement();
      break;
    }

    default:
      break;
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- AdministrationStatementContext ------------------------------------------------------------------

CypherParser::AdministrationStatementContext::AdministrationStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ShowIndexesStatementContext* CypherParser::AdministrationStatementContext::showIndexesStatement() {
  return getRuleContext<CypherParser::ShowIndexesStatementContext>(0);
}

CypherParser::DropIndexStatementContext* CypherParser::AdministrationStatementContext::dropIndexStatement() {
  return getRuleContext<CypherParser::DropIndexStatementContext>(0);
}

CypherParser::CreateIndexStatementContext* CypherParser::AdministrationStatementContext::createIndexStatement() {
  return getRuleContext<CypherParser::CreateIndexStatementContext>(0);
}

CypherParser::CreateConstraintStatementContext* CypherParser::AdministrationStatementContext::createConstraintStatement() {
  return getRuleContext<CypherParser::CreateConstraintStatementContext>(0);
}

CypherParser::DropConstraintStatementContext* CypherParser::AdministrationStatementContext::dropConstraintStatement() {
  return getRuleContext<CypherParser::DropConstraintStatementContext>(0);
}

CypherParser::ShowConstraintsStatementContext* CypherParser::AdministrationStatementContext::showConstraintsStatement() {
  return getRuleContext<CypherParser::ShowConstraintsStatementContext>(0);
}

CypherParser::TransactionStatementContext* CypherParser::AdministrationStatementContext::transactionStatement() {
  return getRuleContext<CypherParser::TransactionStatementContext>(0);
}


size_t CypherParser::AdministrationStatementContext::getRuleIndex() const {
  return CypherParser::RuleAdministrationStatement;
}


std::any CypherParser::AdministrationStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitAdministrationStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::AdministrationStatementContext* CypherParser::administrationStatement() {
  AdministrationStatementContext *_localctx = _tracker.createInstance<AdministrationStatementContext>(_ctx, getState());
  enterRule(_localctx, 4, CypherParser::RuleAdministrationStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(225);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 4, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(218);
      showIndexesStatement();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(219);
      dropIndexStatement();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(220);
      createIndexStatement();
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(221);
      createConstraintStatement();
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(222);
      dropConstraintStatement();
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(223);
      showConstraintsStatement();
      break;
    }

    case 7: {
      enterOuterAlt(_localctx, 7);
      setState(224);
      transactionStatement();
      break;
    }

    default:
      break;
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- TransactionStatementContext ------------------------------------------------------------------

CypherParser::TransactionStatementContext::TransactionStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}


size_t CypherParser::TransactionStatementContext::getRuleIndex() const {
  return CypherParser::RuleTransactionStatement;
}

void CypherParser::TransactionStatementContext::copyFrom(TransactionStatementContext *ctx) {
  ParserRuleContext::copyFrom(ctx);
}

//----------------- TxnRollbackContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::TxnRollbackContext::K_ROLLBACK() {
  return getToken(CypherParser::K_ROLLBACK, 0);
}

tree::TerminalNode* CypherParser::TxnRollbackContext::K_TRANSACTION() {
  return getToken(CypherParser::K_TRANSACTION, 0);
}

CypherParser::TxnRollbackContext::TxnRollbackContext(TransactionStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::TxnRollbackContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitTxnRollback(this);
  else
    return visitor->visitChildren(this);
}
//----------------- TxnCommitContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::TxnCommitContext::K_COMMIT() {
  return getToken(CypherParser::K_COMMIT, 0);
}

tree::TerminalNode* CypherParser::TxnCommitContext::K_TRANSACTION() {
  return getToken(CypherParser::K_TRANSACTION, 0);
}

CypherParser::TxnCommitContext::TxnCommitContext(TransactionStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::TxnCommitContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitTxnCommit(this);
  else
    return visitor->visitChildren(this);
}
//----------------- TxnBeginContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::TxnBeginContext::K_BEGIN() {
  return getToken(CypherParser::K_BEGIN, 0);
}

tree::TerminalNode* CypherParser::TxnBeginContext::K_TRANSACTION() {
  return getToken(CypherParser::K_TRANSACTION, 0);
}

CypherParser::TxnBeginContext::TxnBeginContext(TransactionStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::TxnBeginContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitTxnBegin(this);
  else
    return visitor->visitChildren(this);
}
CypherParser::TransactionStatementContext* CypherParser::transactionStatement() {
  TransactionStatementContext *_localctx = _tracker.createInstance<TransactionStatementContext>(_ctx, getState());
  enterRule(_localctx, 6, CypherParser::RuleTransactionStatement);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(239);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_BEGIN: {
        _localctx = _tracker.createInstance<CypherParser::TxnBeginContext>(_localctx);
        enterOuterAlt(_localctx, 1);
        setState(227);
        match(CypherParser::K_BEGIN);
        setState(229);
        _errHandler->sync(this);

        _la = _input->LA(1);
        if (_la == CypherParser::K_TRANSACTION) {
          setState(228);
          match(CypherParser::K_TRANSACTION);
        }
        break;
      }

      case CypherParser::K_COMMIT: {
        _localctx = _tracker.createInstance<CypherParser::TxnCommitContext>(_localctx);
        enterOuterAlt(_localctx, 2);
        setState(231);
        match(CypherParser::K_COMMIT);
        setState(233);
        _errHandler->sync(this);

        _la = _input->LA(1);
        if (_la == CypherParser::K_TRANSACTION) {
          setState(232);
          match(CypherParser::K_TRANSACTION);
        }
        break;
      }

      case CypherParser::K_ROLLBACK: {
        _localctx = _tracker.createInstance<CypherParser::TxnRollbackContext>(_localctx);
        enterOuterAlt(_localctx, 3);
        setState(235);
        match(CypherParser::K_ROLLBACK);
        setState(237);
        _errHandler->sync(this);

        _la = _input->LA(1);
        if (_la == CypherParser::K_TRANSACTION) {
          setState(236);
          match(CypherParser::K_TRANSACTION);
        }
        break;
      }

    default:
      throw NoViableAltException(this);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ShowIndexesStatementContext ------------------------------------------------------------------

CypherParser::ShowIndexesStatementContext::ShowIndexesStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ShowIndexesStatementContext::K_SHOW() {
  return getToken(CypherParser::K_SHOW, 0);
}

tree::TerminalNode* CypherParser::ShowIndexesStatementContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}


size_t CypherParser::ShowIndexesStatementContext::getRuleIndex() const {
  return CypherParser::RuleShowIndexesStatement;
}


std::any CypherParser::ShowIndexesStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitShowIndexesStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ShowIndexesStatementContext* CypherParser::showIndexesStatement() {
  ShowIndexesStatementContext *_localctx = _tracker.createInstance<ShowIndexesStatementContext>(_ctx, getState());
  enterRule(_localctx, 8, CypherParser::RuleShowIndexesStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(241);
    match(CypherParser::K_SHOW);
    setState(242);
    match(CypherParser::K_INDEX);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- DropIndexStatementContext ------------------------------------------------------------------

CypherParser::DropIndexStatementContext::DropIndexStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}


size_t CypherParser::DropIndexStatementContext::getRuleIndex() const {
  return CypherParser::RuleDropIndexStatement;
}

void CypherParser::DropIndexStatementContext::copyFrom(DropIndexStatementContext *ctx) {
  ParserRuleContext::copyFrom(ctx);
}

//----------------- DropIndexByNameContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::DropIndexByNameContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::DropIndexByNameContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

CypherParser::SymbolicNameContext* CypherParser::DropIndexByNameContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

CypherParser::DropIndexByNameContext::DropIndexByNameContext(DropIndexStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::DropIndexByNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitDropIndexByName(this);
  else
    return visitor->visitChildren(this);
}
//----------------- DropIndexByLabelContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::DropIndexByLabelContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::DropIndexByLabelContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::DropIndexByLabelContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

CypherParser::NodeLabelContext* CypherParser::DropIndexByLabelContext::nodeLabel() {
  return getRuleContext<CypherParser::NodeLabelContext>(0);
}

tree::TerminalNode* CypherParser::DropIndexByLabelContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::DropIndexByLabelContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::DropIndexByLabelContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::DropIndexByLabelContext::DropIndexByLabelContext(DropIndexStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::DropIndexByLabelContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitDropIndexByLabel(this);
  else
    return visitor->visitChildren(this);
}
CypherParser::DropIndexStatementContext* CypherParser::dropIndexStatement() {
  DropIndexStatementContext *_localctx = _tracker.createInstance<DropIndexStatementContext>(_ctx, getState());
  enterRule(_localctx, 10, CypherParser::RuleDropIndexStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(255);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 9, _ctx)) {
    case 1: {
      _localctx = _tracker.createInstance<CypherParser::DropIndexByNameContext>(_localctx);
      enterOuterAlt(_localctx, 1);
      setState(244);
      match(CypherParser::K_DROP);
      setState(245);
      match(CypherParser::K_INDEX);
      setState(246);
      symbolicName();
      break;
    }

    case 2: {
      _localctx = _tracker.createInstance<CypherParser::DropIndexByLabelContext>(_localctx);
      enterOuterAlt(_localctx, 2);
      setState(247);
      match(CypherParser::K_DROP);
      setState(248);
      match(CypherParser::K_INDEX);
      setState(249);
      match(CypherParser::K_ON);
      setState(250);
      nodeLabel();
      setState(251);
      match(CypherParser::LPAREN);
      setState(252);
      propertyKeyName();
      setState(253);
      match(CypherParser::RPAREN);
      break;
    }

    default:
      break;
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- CreateIndexStatementContext ------------------------------------------------------------------

CypherParser::CreateIndexStatementContext::CreateIndexStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}


size_t CypherParser::CreateIndexStatementContext::getRuleIndex() const {
  return CypherParser::RuleCreateIndexStatement;
}

void CypherParser::CreateIndexStatementContext::copyFrom(CreateIndexStatementContext *ctx) {
  ParserRuleContext::copyFrom(ctx);
}

//----------------- CreateIndexByPatternContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::K_FOR() {
  return getToken(CypherParser::K_FOR, 0);
}

CypherParser::NodePatternContext* CypherParser::CreateIndexByPatternContext::nodePattern() {
  return getRuleContext<CypherParser::NodePatternContext>(0);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

std::vector<CypherParser::PropertyExpressionContext *> CypherParser::CreateIndexByPatternContext::propertyExpression() {
  return getRuleContexts<CypherParser::PropertyExpressionContext>();
}

CypherParser::PropertyExpressionContext* CypherParser::CreateIndexByPatternContext::propertyExpression(size_t i) {
  return getRuleContext<CypherParser::PropertyExpressionContext>(i);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::SymbolicNameContext* CypherParser::CreateIndexByPatternContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

std::vector<tree::TerminalNode *> CypherParser::CreateIndexByPatternContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}

CypherParser::CreateIndexByPatternContext::CreateIndexByPatternContext(CreateIndexStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::CreateIndexByPatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateIndexByPattern(this);
  else
    return visitor->visitChildren(this);
}
//----------------- CreateIndexByLabelContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::CreateIndexByLabelContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::CreateIndexByLabelContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::CreateIndexByLabelContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

CypherParser::NodeLabelContext* CypherParser::CreateIndexByLabelContext::nodeLabel() {
  return getRuleContext<CypherParser::NodeLabelContext>(0);
}

tree::TerminalNode* CypherParser::CreateIndexByLabelContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::CreateIndexByLabelContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::CreateIndexByLabelContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::SymbolicNameContext* CypherParser::CreateIndexByLabelContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

CypherParser::CreateIndexByLabelContext::CreateIndexByLabelContext(CreateIndexStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::CreateIndexByLabelContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateIndexByLabel(this);
  else
    return visitor->visitChildren(this);
}
//----------------- CreateVectorIndexContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::CreateVectorIndexContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::K_VECTOR() {
  return getToken(CypherParser::K_VECTOR, 0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

CypherParser::NodeLabelContext* CypherParser::CreateVectorIndexContext::nodeLabel() {
  return getRuleContext<CypherParser::NodeLabelContext>(0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::CreateVectorIndexContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::SymbolicNameContext* CypherParser::CreateVectorIndexContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::K_OPTIONS() {
  return getToken(CypherParser::K_OPTIONS, 0);
}

CypherParser::MapLiteralContext* CypherParser::CreateVectorIndexContext::mapLiteral() {
  return getRuleContext<CypherParser::MapLiteralContext>(0);
}

CypherParser::CreateVectorIndexContext::CreateVectorIndexContext(CreateIndexStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::CreateVectorIndexContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateVectorIndex(this);
  else
    return visitor->visitChildren(this);
}
CypherParser::CreateIndexStatementContext* CypherParser::createIndexStatement() {
  CreateIndexStatementContext *_localctx = _tracker.createInstance<CreateIndexStatementContext>(_ctx, getState());
  enterRule(_localctx, 12, CypherParser::RuleCreateIndexStatement);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(302);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 15, _ctx)) {
    case 1: {
      _localctx = _tracker.createInstance<CypherParser::CreateIndexByPatternContext>(_localctx);
      enterOuterAlt(_localctx, 1);
      setState(257);
      match(CypherParser::K_CREATE);
      setState(258);
      match(CypherParser::K_INDEX);
      setState(260);
      _errHandler->sync(this);

      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 10, _ctx)) {
      case 1: {
        setState(259);
        symbolicName();
        break;
      }

      default:
        break;
      }
      setState(262);
      match(CypherParser::K_FOR);
      setState(263);
      nodePattern();
      setState(264);
      match(CypherParser::K_ON);
      setState(265);
      match(CypherParser::LPAREN);
      setState(266);
      propertyExpression();
      setState(271);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(267);
        match(CypherParser::COMMA);
        setState(268);
        propertyExpression();
        setState(273);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(274);
      match(CypherParser::RPAREN);
      break;
    }

    case 2: {
      _localctx = _tracker.createInstance<CypherParser::CreateIndexByLabelContext>(_localctx);
      enterOuterAlt(_localctx, 2);
      setState(276);
      match(CypherParser::K_CREATE);
      setState(277);
      match(CypherParser::K_INDEX);
      setState(279);
      _errHandler->sync(this);

      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 12, _ctx)) {
      case 1: {
        setState(278);
        symbolicName();
        break;
      }

      default:
        break;
      }
      setState(281);
      match(CypherParser::K_ON);
      setState(282);
      nodeLabel();
      setState(283);
      match(CypherParser::LPAREN);
      setState(284);
      propertyKeyName();
      setState(285);
      match(CypherParser::RPAREN);
      break;
    }

    case 3: {
      _localctx = _tracker.createInstance<CypherParser::CreateVectorIndexContext>(_localctx);
      enterOuterAlt(_localctx, 3);
      setState(287);
      match(CypherParser::K_CREATE);
      setState(288);
      match(CypherParser::K_VECTOR);
      setState(289);
      match(CypherParser::K_INDEX);
      setState(291);
      _errHandler->sync(this);

      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 13, _ctx)) {
      case 1: {
        setState(290);
        symbolicName();
        break;
      }

      default:
        break;
      }
      setState(293);
      match(CypherParser::K_ON);
      setState(294);
      nodeLabel();
      setState(295);
      match(CypherParser::LPAREN);
      setState(296);
      propertyKeyName();
      setState(297);
      match(CypherParser::RPAREN);
      setState(300);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_OPTIONS) {
        setState(298);
        match(CypherParser::K_OPTIONS);
        setState(299);
        mapLiteral();
      }
      break;
    }

    default:
      break;
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- CreateConstraintStatementContext ------------------------------------------------------------------

CypherParser::CreateConstraintStatementContext::CreateConstraintStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}


size_t CypherParser::CreateConstraintStatementContext::getRuleIndex() const {
  return CypherParser::RuleCreateConstraintStatement;
}

void CypherParser::CreateConstraintStatementContext::copyFrom(CreateConstraintStatementContext *ctx) {
  ParserRuleContext::copyFrom(ctx);
}

//----------------- CreateNodeConstraintContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::CreateNodeConstraintContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::CreateNodeConstraintContext::K_CONSTRAINT() {
  return getToken(CypherParser::K_CONSTRAINT, 0);
}

CypherParser::SymbolicNameContext* CypherParser::CreateNodeConstraintContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

tree::TerminalNode* CypherParser::CreateNodeConstraintContext::K_FOR() {
  return getToken(CypherParser::K_FOR, 0);
}

CypherParser::ConstraintNodePatternContext* CypherParser::CreateNodeConstraintContext::constraintNodePattern() {
  return getRuleContext<CypherParser::ConstraintNodePatternContext>(0);
}

tree::TerminalNode* CypherParser::CreateNodeConstraintContext::K_REQUIRE() {
  return getToken(CypherParser::K_REQUIRE, 0);
}

CypherParser::ConstraintBodyContext* CypherParser::CreateNodeConstraintContext::constraintBody() {
  return getRuleContext<CypherParser::ConstraintBodyContext>(0);
}

CypherParser::CreateNodeConstraintContext::CreateNodeConstraintContext(CreateConstraintStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::CreateNodeConstraintContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateNodeConstraint(this);
  else
    return visitor->visitChildren(this);
}
//----------------- CreateEdgeConstraintContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::CreateEdgeConstraintContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::CreateEdgeConstraintContext::K_CONSTRAINT() {
  return getToken(CypherParser::K_CONSTRAINT, 0);
}

CypherParser::SymbolicNameContext* CypherParser::CreateEdgeConstraintContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

tree::TerminalNode* CypherParser::CreateEdgeConstraintContext::K_FOR() {
  return getToken(CypherParser::K_FOR, 0);
}

CypherParser::ConstraintEdgePatternContext* CypherParser::CreateEdgeConstraintContext::constraintEdgePattern() {
  return getRuleContext<CypherParser::ConstraintEdgePatternContext>(0);
}

tree::TerminalNode* CypherParser::CreateEdgeConstraintContext::K_REQUIRE() {
  return getToken(CypherParser::K_REQUIRE, 0);
}

CypherParser::ConstraintBodyContext* CypherParser::CreateEdgeConstraintContext::constraintBody() {
  return getRuleContext<CypherParser::ConstraintBodyContext>(0);
}

CypherParser::CreateEdgeConstraintContext::CreateEdgeConstraintContext(CreateConstraintStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::CreateEdgeConstraintContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateEdgeConstraint(this);
  else
    return visitor->visitChildren(this);
}
CypherParser::CreateConstraintStatementContext* CypherParser::createConstraintStatement() {
  CreateConstraintStatementContext *_localctx = _tracker.createInstance<CreateConstraintStatementContext>(_ctx, getState());
  enterRule(_localctx, 14, CypherParser::RuleCreateConstraintStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(320);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 16, _ctx)) {
    case 1: {
      _localctx = _tracker.createInstance<CypherParser::CreateNodeConstraintContext>(_localctx);
      enterOuterAlt(_localctx, 1);
      setState(304);
      match(CypherParser::K_CREATE);
      setState(305);
      match(CypherParser::K_CONSTRAINT);
      setState(306);
      symbolicName();
      setState(307);
      match(CypherParser::K_FOR);
      setState(308);
      constraintNodePattern();
      setState(309);
      match(CypherParser::K_REQUIRE);
      setState(310);
      constraintBody();
      break;
    }

    case 2: {
      _localctx = _tracker.createInstance<CypherParser::CreateEdgeConstraintContext>(_localctx);
      enterOuterAlt(_localctx, 2);
      setState(312);
      match(CypherParser::K_CREATE);
      setState(313);
      match(CypherParser::K_CONSTRAINT);
      setState(314);
      symbolicName();
      setState(315);
      match(CypherParser::K_FOR);
      setState(316);
      constraintEdgePattern();
      setState(317);
      match(CypherParser::K_REQUIRE);
      setState(318);
      constraintBody();
      break;
    }

    default:
      break;
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ConstraintNodePatternContext ------------------------------------------------------------------

CypherParser::ConstraintNodePatternContext::ConstraintNodePatternContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ConstraintNodePatternContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::VariableContext* CypherParser::ConstraintNodePatternContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::ConstraintNodePatternContext::COLON() {
  return getToken(CypherParser::COLON, 0);
}

CypherParser::LabelNameContext* CypherParser::ConstraintNodePatternContext::labelName() {
  return getRuleContext<CypherParser::LabelNameContext>(0);
}

tree::TerminalNode* CypherParser::ConstraintNodePatternContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}


size_t CypherParser::ConstraintNodePatternContext::getRuleIndex() const {
  return CypherParser::RuleConstraintNodePattern;
}


std::any CypherParser::ConstraintNodePatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitConstraintNodePattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ConstraintNodePatternContext* CypherParser::constraintNodePattern() {
  ConstraintNodePatternContext *_localctx = _tracker.createInstance<ConstraintNodePatternContext>(_ctx, getState());
  enterRule(_localctx, 16, CypherParser::RuleConstraintNodePattern);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(322);
    match(CypherParser::LPAREN);
    setState(323);
    variable();
    setState(324);
    match(CypherParser::COLON);
    setState(325);
    labelName();
    setState(326);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ConstraintEdgePatternContext ------------------------------------------------------------------

CypherParser::ConstraintEdgePatternContext::ConstraintEdgePatternContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<tree::TerminalNode *> CypherParser::ConstraintEdgePatternContext::LPAREN() {
  return getTokens(CypherParser::LPAREN);
}

tree::TerminalNode* CypherParser::ConstraintEdgePatternContext::LPAREN(size_t i) {
  return getToken(CypherParser::LPAREN, i);
}

std::vector<tree::TerminalNode *> CypherParser::ConstraintEdgePatternContext::RPAREN() {
  return getTokens(CypherParser::RPAREN);
}

tree::TerminalNode* CypherParser::ConstraintEdgePatternContext::RPAREN(size_t i) {
  return getToken(CypherParser::RPAREN, i);
}

std::vector<tree::TerminalNode *> CypherParser::ConstraintEdgePatternContext::MINUS() {
  return getTokens(CypherParser::MINUS);
}

tree::TerminalNode* CypherParser::ConstraintEdgePatternContext::MINUS(size_t i) {
  return getToken(CypherParser::MINUS, i);
}

tree::TerminalNode* CypherParser::ConstraintEdgePatternContext::LBRACK() {
  return getToken(CypherParser::LBRACK, 0);
}

CypherParser::VariableContext* CypherParser::ConstraintEdgePatternContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::ConstraintEdgePatternContext::COLON() {
  return getToken(CypherParser::COLON, 0);
}

CypherParser::LabelNameContext* CypherParser::ConstraintEdgePatternContext::labelName() {
  return getRuleContext<CypherParser::LabelNameContext>(0);
}

tree::TerminalNode* CypherParser::ConstraintEdgePatternContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}


size_t CypherParser::ConstraintEdgePatternContext::getRuleIndex() const {
  return CypherParser::RuleConstraintEdgePattern;
}


std::any CypherParser::ConstraintEdgePatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitConstraintEdgePattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ConstraintEdgePatternContext* CypherParser::constraintEdgePattern() {
  ConstraintEdgePatternContext *_localctx = _tracker.createInstance<ConstraintEdgePatternContext>(_ctx, getState());
  enterRule(_localctx, 18, CypherParser::RuleConstraintEdgePattern);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(328);
    match(CypherParser::LPAREN);
    setState(329);
    match(CypherParser::RPAREN);
    setState(330);
    match(CypherParser::MINUS);
    setState(331);
    match(CypherParser::LBRACK);
    setState(332);
    variable();
    setState(333);
    match(CypherParser::COLON);
    setState(334);
    labelName();
    setState(335);
    match(CypherParser::RBRACK);
    setState(336);
    match(CypherParser::MINUS);
    setState(337);
    match(CypherParser::LPAREN);
    setState(338);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ConstraintBodyContext ------------------------------------------------------------------

CypherParser::ConstraintBodyContext::ConstraintBodyContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}


size_t CypherParser::ConstraintBodyContext::getRuleIndex() const {
  return CypherParser::RuleConstraintBody;
}

void CypherParser::ConstraintBodyContext::copyFrom(ConstraintBodyContext *ctx) {
  ParserRuleContext::copyFrom(ctx);
}

//----------------- NodeKeyConstraintBodyContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::NodeKeyConstraintBodyContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

std::vector<CypherParser::PropertyExpressionContext *> CypherParser::NodeKeyConstraintBodyContext::propertyExpression() {
  return getRuleContexts<CypherParser::PropertyExpressionContext>();
}

CypherParser::PropertyExpressionContext* CypherParser::NodeKeyConstraintBodyContext::propertyExpression(size_t i) {
  return getRuleContext<CypherParser::PropertyExpressionContext>(i);
}

tree::TerminalNode* CypherParser::NodeKeyConstraintBodyContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

tree::TerminalNode* CypherParser::NodeKeyConstraintBodyContext::K_IS() {
  return getToken(CypherParser::K_IS, 0);
}

tree::TerminalNode* CypherParser::NodeKeyConstraintBodyContext::K_NODE() {
  return getToken(CypherParser::K_NODE, 0);
}

tree::TerminalNode* CypherParser::NodeKeyConstraintBodyContext::K_KEY() {
  return getToken(CypherParser::K_KEY, 0);
}

std::vector<tree::TerminalNode *> CypherParser::NodeKeyConstraintBodyContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::NodeKeyConstraintBodyContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}

CypherParser::NodeKeyConstraintBodyContext::NodeKeyConstraintBodyContext(ConstraintBodyContext *ctx) { copyFrom(ctx); }


std::any CypherParser::NodeKeyConstraintBodyContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNodeKeyConstraintBody(this);
  else
    return visitor->visitChildren(this);
}
//----------------- UniqueConstraintBodyContext ------------------------------------------------------------------

CypherParser::PropertyExpressionContext* CypherParser::UniqueConstraintBodyContext::propertyExpression() {
  return getRuleContext<CypherParser::PropertyExpressionContext>(0);
}

tree::TerminalNode* CypherParser::UniqueConstraintBodyContext::K_IS() {
  return getToken(CypherParser::K_IS, 0);
}

tree::TerminalNode* CypherParser::UniqueConstraintBodyContext::K_UNIQUE() {
  return getToken(CypherParser::K_UNIQUE, 0);
}

CypherParser::UniqueConstraintBodyContext::UniqueConstraintBodyContext(ConstraintBodyContext *ctx) { copyFrom(ctx); }


std::any CypherParser::UniqueConstraintBodyContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitUniqueConstraintBody(this);
  else
    return visitor->visitChildren(this);
}
//----------------- PropertyTypeConstraintBodyContext ------------------------------------------------------------------

CypherParser::PropertyExpressionContext* CypherParser::PropertyTypeConstraintBodyContext::propertyExpression() {
  return getRuleContext<CypherParser::PropertyExpressionContext>(0);
}

tree::TerminalNode* CypherParser::PropertyTypeConstraintBodyContext::K_IS() {
  return getToken(CypherParser::K_IS, 0);
}

std::vector<tree::TerminalNode *> CypherParser::PropertyTypeConstraintBodyContext::COLON() {
  return getTokens(CypherParser::COLON);
}

tree::TerminalNode* CypherParser::PropertyTypeConstraintBodyContext::COLON(size_t i) {
  return getToken(CypherParser::COLON, i);
}

CypherParser::ConstraintTypeNameContext* CypherParser::PropertyTypeConstraintBodyContext::constraintTypeName() {
  return getRuleContext<CypherParser::ConstraintTypeNameContext>(0);
}

CypherParser::PropertyTypeConstraintBodyContext::PropertyTypeConstraintBodyContext(ConstraintBodyContext *ctx) { copyFrom(ctx); }


std::any CypherParser::PropertyTypeConstraintBodyContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPropertyTypeConstraintBody(this);
  else
    return visitor->visitChildren(this);
}
//----------------- NotNullConstraintBodyContext ------------------------------------------------------------------

CypherParser::PropertyExpressionContext* CypherParser::NotNullConstraintBodyContext::propertyExpression() {
  return getRuleContext<CypherParser::PropertyExpressionContext>(0);
}

tree::TerminalNode* CypherParser::NotNullConstraintBodyContext::K_IS() {
  return getToken(CypherParser::K_IS, 0);
}

tree::TerminalNode* CypherParser::NotNullConstraintBodyContext::K_NOT() {
  return getToken(CypherParser::K_NOT, 0);
}

tree::TerminalNode* CypherParser::NotNullConstraintBodyContext::K_NULL() {
  return getToken(CypherParser::K_NULL, 0);
}

CypherParser::NotNullConstraintBodyContext::NotNullConstraintBodyContext(ConstraintBodyContext *ctx) { copyFrom(ctx); }


std::any CypherParser::NotNullConstraintBodyContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNotNullConstraintBody(this);
  else
    return visitor->visitChildren(this);
}
CypherParser::ConstraintBodyContext* CypherParser::constraintBody() {
  ConstraintBodyContext *_localctx = _tracker.createInstance<ConstraintBodyContext>(_ctx, getState());
  enterRule(_localctx, 20, CypherParser::RuleConstraintBody);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(369);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 18, _ctx)) {
    case 1: {
      _localctx = _tracker.createInstance<CypherParser::UniqueConstraintBodyContext>(_localctx);
      enterOuterAlt(_localctx, 1);
      setState(340);
      propertyExpression();
      setState(341);
      match(CypherParser::K_IS);
      setState(342);
      match(CypherParser::K_UNIQUE);
      break;
    }

    case 2: {
      _localctx = _tracker.createInstance<CypherParser::NotNullConstraintBodyContext>(_localctx);
      enterOuterAlt(_localctx, 2);
      setState(344);
      propertyExpression();
      setState(345);
      match(CypherParser::K_IS);
      setState(346);
      match(CypherParser::K_NOT);
      setState(347);
      match(CypherParser::K_NULL);
      break;
    }

    case 3: {
      _localctx = _tracker.createInstance<CypherParser::PropertyTypeConstraintBodyContext>(_localctx);
      enterOuterAlt(_localctx, 3);
      setState(349);
      propertyExpression();
      setState(350);
      match(CypherParser::K_IS);
      setState(351);
      match(CypherParser::COLON);
      setState(352);
      match(CypherParser::COLON);
      setState(353);
      constraintTypeName();
      break;
    }

    case 4: {
      _localctx = _tracker.createInstance<CypherParser::NodeKeyConstraintBodyContext>(_localctx);
      enterOuterAlt(_localctx, 4);
      setState(355);
      match(CypherParser::LPAREN);
      setState(356);
      propertyExpression();
      setState(361);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(357);
        match(CypherParser::COMMA);
        setState(358);
        propertyExpression();
        setState(363);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(364);
      match(CypherParser::RPAREN);
      setState(365);
      match(CypherParser::K_IS);
      setState(366);
      match(CypherParser::K_NODE);
      setState(367);
      match(CypherParser::K_KEY);
      break;
    }

    default:
      break;
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ConstraintTypeNameContext ------------------------------------------------------------------

CypherParser::ConstraintTypeNameContext::ConstraintTypeNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ConstraintTypeNameContext::K_BOOLEAN() {
  return getToken(CypherParser::K_BOOLEAN, 0);
}

tree::TerminalNode* CypherParser::ConstraintTypeNameContext::K_INTEGER() {
  return getToken(CypherParser::K_INTEGER, 0);
}

tree::TerminalNode* CypherParser::ConstraintTypeNameContext::K_FLOAT() {
  return getToken(CypherParser::K_FLOAT, 0);
}

tree::TerminalNode* CypherParser::ConstraintTypeNameContext::K_STRING() {
  return getToken(CypherParser::K_STRING, 0);
}

tree::TerminalNode* CypherParser::ConstraintTypeNameContext::K_LIST() {
  return getToken(CypherParser::K_LIST, 0);
}

tree::TerminalNode* CypherParser::ConstraintTypeNameContext::K_MAP() {
  return getToken(CypherParser::K_MAP, 0);
}


size_t CypherParser::ConstraintTypeNameContext::getRuleIndex() const {
  return CypherParser::RuleConstraintTypeName;
}


std::any CypherParser::ConstraintTypeNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitConstraintTypeName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ConstraintTypeNameContext* CypherParser::constraintTypeName() {
  ConstraintTypeNameContext *_localctx = _tracker.createInstance<ConstraintTypeNameContext>(_ctx, getState());
  enterRule(_localctx, 22, CypherParser::RuleConstraintTypeName);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(371);
    _la = _input->LA(1);
    if (!(((((_la - 75) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 75)) & 63) != 0))) {
    _errHandler->recoverInline(this);
    }
    else {
      _errHandler->reportMatch(this);
      consume();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- DropConstraintStatementContext ------------------------------------------------------------------

CypherParser::DropConstraintStatementContext::DropConstraintStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}


size_t CypherParser::DropConstraintStatementContext::getRuleIndex() const {
  return CypherParser::RuleDropConstraintStatement;
}

void CypherParser::DropConstraintStatementContext::copyFrom(DropConstraintStatementContext *ctx) {
  ParserRuleContext::copyFrom(ctx);
}

//----------------- DropConstraintIfExistsContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::DropConstraintIfExistsContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::DropConstraintIfExistsContext::K_CONSTRAINT() {
  return getToken(CypherParser::K_CONSTRAINT, 0);
}

CypherParser::SymbolicNameContext* CypherParser::DropConstraintIfExistsContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

tree::TerminalNode* CypherParser::DropConstraintIfExistsContext::K_IF() {
  return getToken(CypherParser::K_IF, 0);
}

tree::TerminalNode* CypherParser::DropConstraintIfExistsContext::K_EXISTS() {
  return getToken(CypherParser::K_EXISTS, 0);
}

CypherParser::DropConstraintIfExistsContext::DropConstraintIfExistsContext(DropConstraintStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::DropConstraintIfExistsContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitDropConstraintIfExists(this);
  else
    return visitor->visitChildren(this);
}
//----------------- DropConstraintByNameContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::DropConstraintByNameContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::DropConstraintByNameContext::K_CONSTRAINT() {
  return getToken(CypherParser::K_CONSTRAINT, 0);
}

CypherParser::SymbolicNameContext* CypherParser::DropConstraintByNameContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

CypherParser::DropConstraintByNameContext::DropConstraintByNameContext(DropConstraintStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::DropConstraintByNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitDropConstraintByName(this);
  else
    return visitor->visitChildren(this);
}
CypherParser::DropConstraintStatementContext* CypherParser::dropConstraintStatement() {
  DropConstraintStatementContext *_localctx = _tracker.createInstance<DropConstraintStatementContext>(_ctx, getState());
  enterRule(_localctx, 24, CypherParser::RuleDropConstraintStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(382);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 19, _ctx)) {
    case 1: {
      _localctx = _tracker.createInstance<CypherParser::DropConstraintByNameContext>(_localctx);
      enterOuterAlt(_localctx, 1);
      setState(373);
      match(CypherParser::K_DROP);
      setState(374);
      match(CypherParser::K_CONSTRAINT);
      setState(375);
      symbolicName();
      break;
    }

    case 2: {
      _localctx = _tracker.createInstance<CypherParser::DropConstraintIfExistsContext>(_localctx);
      enterOuterAlt(_localctx, 2);
      setState(376);
      match(CypherParser::K_DROP);
      setState(377);
      match(CypherParser::K_CONSTRAINT);
      setState(378);
      symbolicName();
      setState(379);
      match(CypherParser::K_IF);
      setState(380);
      match(CypherParser::K_EXISTS);
      break;
    }

    default:
      break;
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ShowConstraintsStatementContext ------------------------------------------------------------------

CypherParser::ShowConstraintsStatementContext::ShowConstraintsStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ShowConstraintsStatementContext::K_SHOW() {
  return getToken(CypherParser::K_SHOW, 0);
}

tree::TerminalNode* CypherParser::ShowConstraintsStatementContext::K_CONSTRAINT() {
  return getToken(CypherParser::K_CONSTRAINT, 0);
}


size_t CypherParser::ShowConstraintsStatementContext::getRuleIndex() const {
  return CypherParser::RuleShowConstraintsStatement;
}


std::any CypherParser::ShowConstraintsStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitShowConstraintsStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ShowConstraintsStatementContext* CypherParser::showConstraintsStatement() {
  ShowConstraintsStatementContext *_localctx = _tracker.createInstance<ShowConstraintsStatementContext>(_ctx, getState());
  enterRule(_localctx, 26, CypherParser::RuleShowConstraintsStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(384);
    match(CypherParser::K_SHOW);
    setState(385);
    match(CypherParser::K_CONSTRAINT);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

