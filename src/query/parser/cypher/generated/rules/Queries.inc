//----------------- QueryContext ------------------------------------------------------------------

CypherParser::QueryContext::QueryContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::RegularQueryContext* CypherParser::QueryContext::regularQuery() {
  return getRuleContext<CypherParser::RegularQueryContext>(0);
}

CypherParser::StandaloneCallStatementContext* CypherParser::QueryContext::standaloneCallStatement() {
  return getRuleContext<CypherParser::StandaloneCallStatementContext>(0);
}


size_t CypherParser::QueryContext::getRuleIndex() const {
  return CypherParser::RuleQuery;
}


std::any CypherParser::QueryContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitQuery(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::QueryContext* CypherParser::query() {
  QueryContext *_localctx = _tracker.createInstance<QueryContext>(_ctx, getState());
  enterRule(_localctx, 28, CypherParser::RuleQuery);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(389);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 20, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(387);
      regularQuery();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(388);
      standaloneCallStatement();
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

//----------------- RegularQueryContext ------------------------------------------------------------------

CypherParser::RegularQueryContext::RegularQueryContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::SingleQueryContext *> CypherParser::RegularQueryContext::singleQuery() {
  return getRuleContexts<CypherParser::SingleQueryContext>();
}

CypherParser::SingleQueryContext* CypherParser::RegularQueryContext::singleQuery(size_t i) {
  return getRuleContext<CypherParser::SingleQueryContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::RegularQueryContext::K_UNION() {
  return getTokens(CypherParser::K_UNION);
}

tree::TerminalNode* CypherParser::RegularQueryContext::K_UNION(size_t i) {
  return getToken(CypherParser::K_UNION, i);
}

std::vector<tree::TerminalNode *> CypherParser::RegularQueryContext::K_ALL() {
  return getTokens(CypherParser::K_ALL);
}

tree::TerminalNode* CypherParser::RegularQueryContext::K_ALL(size_t i) {
  return getToken(CypherParser::K_ALL, i);
}


size_t CypherParser::RegularQueryContext::getRuleIndex() const {
  return CypherParser::RuleRegularQuery;
}


std::any CypherParser::RegularQueryContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRegularQuery(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RegularQueryContext* CypherParser::regularQuery() {
  RegularQueryContext *_localctx = _tracker.createInstance<RegularQueryContext>(_ctx, getState());
  enterRule(_localctx, 30, CypherParser::RuleRegularQuery);
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
    setState(391);
    singleQuery();
    setState(399);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_UNION) {
      setState(392);
      match(CypherParser::K_UNION);
      setState(394);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_ALL) {
        setState(393);
        match(CypherParser::K_ALL);
      }
      setState(396);
      singleQuery();
      setState(401);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- SingleQueryContext ------------------------------------------------------------------

CypherParser::SingleQueryContext::SingleQueryContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ReturnStatementContext* CypherParser::SingleQueryContext::returnStatement() {
  return getRuleContext<CypherParser::ReturnStatementContext>(0);
}

std::vector<CypherParser::WithClauseContext *> CypherParser::SingleQueryContext::withClause() {
  return getRuleContexts<CypherParser::WithClauseContext>();
}

CypherParser::WithClauseContext* CypherParser::SingleQueryContext::withClause(size_t i) {
  return getRuleContext<CypherParser::WithClauseContext>(i);
}

std::vector<CypherParser::ReadingClauseContext *> CypherParser::SingleQueryContext::readingClause() {
  return getRuleContexts<CypherParser::ReadingClauseContext>();
}

CypherParser::ReadingClauseContext* CypherParser::SingleQueryContext::readingClause(size_t i) {
  return getRuleContext<CypherParser::ReadingClauseContext>(i);
}

std::vector<CypherParser::UpdatingClauseContext *> CypherParser::SingleQueryContext::updatingClause() {
  return getRuleContexts<CypherParser::UpdatingClauseContext>();
}

CypherParser::UpdatingClauseContext* CypherParser::SingleQueryContext::updatingClause(size_t i) {
  return getRuleContext<CypherParser::UpdatingClauseContext>(i);
}

std::vector<CypherParser::CallSubqueryContext *> CypherParser::SingleQueryContext::callSubquery() {
  return getRuleContexts<CypherParser::CallSubqueryContext>();
}

CypherParser::CallSubqueryContext* CypherParser::SingleQueryContext::callSubquery(size_t i) {
  return getRuleContext<CypherParser::CallSubqueryContext>(i);
}


size_t CypherParser::SingleQueryContext::getRuleIndex() const {
  return CypherParser::RuleSingleQuery;
}


std::any CypherParser::SingleQueryContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSingleQuery(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SingleQueryContext* CypherParser::singleQuery() {
  SingleQueryContext *_localctx = _tracker.createInstance<SingleQueryContext>(_ctx, getState());
  enterRule(_localctx, 32, CypherParser::RuleSingleQuery);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    size_t alt;
    setState(501);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 43, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(403); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(402);
        withClause();
        setState(405); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(410);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0) || _la == CypherParser::K_LOAD) {
        setState(407);
        readingClause();
        setState(412);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(413);
      returnStatement();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(416); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(415);
        withClause();
        setState(418); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(423);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0) || _la == CypherParser::K_LOAD) {
        setState(420);
        readingClause();
        setState(425);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(427); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(426);
        updatingClause();
        setState(429); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 1311288) != 0) || _la == CypherParser::K_FOREACH);
      setState(432);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_RETURN) {
        setState(431);
        returnStatement();
      }
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(437);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0) || _la == CypherParser::K_LOAD) {
        setState(434);
        readingClause();
        setState(439);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(441); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(440);
        withClause();
        setState(443); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(448);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0) || _la == CypherParser::K_LOAD) {
        setState(445);
        readingClause();
        setState(450);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(451);
      returnStatement();
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(456);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0) || _la == CypherParser::K_LOAD) {
        setState(453);
        readingClause();
        setState(458);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(460); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(459);
        withClause();
        setState(462); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(467);
      _errHandler->sync(this);
      alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 34, _ctx);
      while (alt != 2 && alt != atn::ATN::INVALID_ALT_NUMBER) {
        if (alt == 1) {
          setState(464);
          readingClause(); 
        }
        setState(469);
        _errHandler->sync(this);
        alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 34, _ctx);
      }
      setState(472); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(472);
        _errHandler->sync(this);
        switch (_input->LA(1)) {
          case CypherParser::K_CREATE:
          case CypherParser::K_DELETE:
          case CypherParser::K_DETACH:
          case CypherParser::K_MERGE:
          case CypherParser::K_REMOVE:
          case CypherParser::K_SET:
          case CypherParser::K_FOREACH: {
            setState(470);
            updatingClause();
            break;
          }

          case CypherParser::K_CALL: {
            setState(471);
            callSubquery();
            break;
          }

        default:
          throw NoViableAltException(this);
        }
        setState(474); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 1311290) != 0) || _la == CypherParser::K_FOREACH);
      setState(477);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_RETURN) {
        setState(476);
        returnStatement();
      }
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(482);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0) || _la == CypherParser::K_LOAD) {
        setState(479);
        readingClause();
        setState(484);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(485);
      returnStatement();
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(489);
      _errHandler->sync(this);
      alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 39, _ctx);
      while (alt != 2 && alt != atn::ATN::INVALID_ALT_NUMBER) {
        if (alt == 1) {
          setState(486);
          readingClause(); 
        }
        setState(491);
        _errHandler->sync(this);
        alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 39, _ctx);
      }
      setState(494); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(494);
        _errHandler->sync(this);
        switch (_input->LA(1)) {
          case CypherParser::K_CREATE:
          case CypherParser::K_DELETE:
          case CypherParser::K_DETACH:
          case CypherParser::K_MERGE:
          case CypherParser::K_REMOVE:
          case CypherParser::K_SET:
          case CypherParser::K_FOREACH: {
            setState(492);
            updatingClause();
            break;
          }

          case CypherParser::K_CALL: {
            setState(493);
            callSubquery();
            break;
          }

        default:
          throw NoViableAltException(this);
        }
        setState(496); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 1311290) != 0) || _la == CypherParser::K_FOREACH);
      setState(499);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_RETURN) {
        setState(498);
        returnStatement();
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

//----------------- ReadingClauseContext ------------------------------------------------------------------

CypherParser::ReadingClauseContext::ReadingClauseContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::MatchStatementContext* CypherParser::ReadingClauseContext::matchStatement() {
  return getRuleContext<CypherParser::MatchStatementContext>(0);
}

CypherParser::UnwindStatementContext* CypherParser::ReadingClauseContext::unwindStatement() {
  return getRuleContext<CypherParser::UnwindStatementContext>(0);
}

CypherParser::CallSubqueryContext* CypherParser::ReadingClauseContext::callSubquery() {
  return getRuleContext<CypherParser::CallSubqueryContext>(0);
}

CypherParser::InQueryCallStatementContext* CypherParser::ReadingClauseContext::inQueryCallStatement() {
  return getRuleContext<CypherParser::InQueryCallStatementContext>(0);
}

CypherParser::LoadCsvStatementContext* CypherParser::ReadingClauseContext::loadCsvStatement() {
  return getRuleContext<CypherParser::LoadCsvStatementContext>(0);
}


size_t CypherParser::ReadingClauseContext::getRuleIndex() const {
  return CypherParser::RuleReadingClause;
}


std::any CypherParser::ReadingClauseContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitReadingClause(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ReadingClauseContext* CypherParser::readingClause() {
  ReadingClauseContext *_localctx = _tracker.createInstance<ReadingClauseContext>(_ctx, getState());
  enterRule(_localctx, 34, CypherParser::RuleReadingClause);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(508);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 44, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(503);
      matchStatement();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(504);
      unwindStatement();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(505);
      callSubquery();
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(506);
      inQueryCallStatement();
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(507);
      loadCsvStatement();
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

//----------------- UpdatingClauseContext ------------------------------------------------------------------

CypherParser::UpdatingClauseContext::UpdatingClauseContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::CreateStatementContext* CypherParser::UpdatingClauseContext::createStatement() {
  return getRuleContext<CypherParser::CreateStatementContext>(0);
}

CypherParser::MergeStatementContext* CypherParser::UpdatingClauseContext::mergeStatement() {
  return getRuleContext<CypherParser::MergeStatementContext>(0);
}

CypherParser::DeleteStatementContext* CypherParser::UpdatingClauseContext::deleteStatement() {
  return getRuleContext<CypherParser::DeleteStatementContext>(0);
}

CypherParser::SetStatementContext* CypherParser::UpdatingClauseContext::setStatement() {
  return getRuleContext<CypherParser::SetStatementContext>(0);
}

CypherParser::RemoveStatementContext* CypherParser::UpdatingClauseContext::removeStatement() {
  return getRuleContext<CypherParser::RemoveStatementContext>(0);
}

CypherParser::ForeachStatementContext* CypherParser::UpdatingClauseContext::foreachStatement() {
  return getRuleContext<CypherParser::ForeachStatementContext>(0);
}


size_t CypherParser::UpdatingClauseContext::getRuleIndex() const {
  return CypherParser::RuleUpdatingClause;
}


std::any CypherParser::UpdatingClauseContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitUpdatingClause(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::UpdatingClauseContext* CypherParser::updatingClause() {
  UpdatingClauseContext *_localctx = _tracker.createInstance<UpdatingClauseContext>(_ctx, getState());
  enterRule(_localctx, 36, CypherParser::RuleUpdatingClause);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(516);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 1);
        setState(510);
        createStatement();
        break;
      }

      case CypherParser::K_MERGE: {
        enterOuterAlt(_localctx, 2);
        setState(511);
        mergeStatement();
        break;
      }

      case CypherParser::K_DELETE:
      case CypherParser::K_DETACH: {
        enterOuterAlt(_localctx, 3);
        setState(512);
        deleteStatement();
        break;
      }

      case CypherParser::K_SET: {
        enterOuterAlt(_localctx, 4);
        setState(513);
        setStatement();
        break;
      }

      case CypherParser::K_REMOVE: {
        enterOuterAlt(_localctx, 5);
        setState(514);
        removeStatement();
        break;
      }

      case CypherParser::K_FOREACH: {
        enterOuterAlt(_localctx, 6);
        setState(515);
        foreachStatement();
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

//----------------- WithClauseContext ------------------------------------------------------------------

CypherParser::WithClauseContext::WithClauseContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::WithClauseContext::K_WITH() {
  return getToken(CypherParser::K_WITH, 0);
}

CypherParser::ProjectionBodyContext* CypherParser::WithClauseContext::projectionBody() {
  return getRuleContext<CypherParser::ProjectionBodyContext>(0);
}

tree::TerminalNode* CypherParser::WithClauseContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

CypherParser::WhereContext* CypherParser::WithClauseContext::where() {
  return getRuleContext<CypherParser::WhereContext>(0);
}


size_t CypherParser::WithClauseContext::getRuleIndex() const {
  return CypherParser::RuleWithClause;
}


std::any CypherParser::WithClauseContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitWithClause(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::WithClauseContext* CypherParser::withClause() {
  WithClauseContext *_localctx = _tracker.createInstance<WithClauseContext>(_ctx, getState());
  enterRule(_localctx, 38, CypherParser::RuleWithClause);
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
    setState(518);
    match(CypherParser::K_WITH);
    setState(519);
    projectionBody();
    setState(522);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(520);
      match(CypherParser::K_WHERE);
      setState(521);
      where();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- MatchStatementContext ------------------------------------------------------------------

CypherParser::MatchStatementContext::MatchStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::MatchStatementContext::K_MATCH() {
  return getToken(CypherParser::K_MATCH, 0);
}

CypherParser::PatternContext* CypherParser::MatchStatementContext::pattern() {
  return getRuleContext<CypherParser::PatternContext>(0);
}

tree::TerminalNode* CypherParser::MatchStatementContext::K_OPTIONAL() {
  return getToken(CypherParser::K_OPTIONAL, 0);
}

tree::TerminalNode* CypherParser::MatchStatementContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

CypherParser::WhereContext* CypherParser::MatchStatementContext::where() {
  return getRuleContext<CypherParser::WhereContext>(0);
}


size_t CypherParser::MatchStatementContext::getRuleIndex() const {
  return CypherParser::RuleMatchStatement;
}


std::any CypherParser::MatchStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitMatchStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MatchStatementContext* CypherParser::matchStatement() {
  MatchStatementContext *_localctx = _tracker.createInstance<MatchStatementContext>(_ctx, getState());
  enterRule(_localctx, 40, CypherParser::RuleMatchStatement);
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
    setState(525);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_OPTIONAL) {
      setState(524);
      match(CypherParser::K_OPTIONAL);
    }
    setState(527);
    match(CypherParser::K_MATCH);
    setState(528);
    pattern();
    setState(531);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(529);
      match(CypherParser::K_WHERE);
      setState(530);
      where();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- UnwindStatementContext ------------------------------------------------------------------

CypherParser::UnwindStatementContext::UnwindStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::UnwindStatementContext::K_UNWIND() {
  return getToken(CypherParser::K_UNWIND, 0);
}

CypherParser::ExpressionContext* CypherParser::UnwindStatementContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::UnwindStatementContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}

CypherParser::VariableContext* CypherParser::UnwindStatementContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}


size_t CypherParser::UnwindStatementContext::getRuleIndex() const {
  return CypherParser::RuleUnwindStatement;
}


std::any CypherParser::UnwindStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitUnwindStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::UnwindStatementContext* CypherParser::unwindStatement() {
  UnwindStatementContext *_localctx = _tracker.createInstance<UnwindStatementContext>(_ctx, getState());
  enterRule(_localctx, 42, CypherParser::RuleUnwindStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(533);
    match(CypherParser::K_UNWIND);
    setState(534);
    expression();
    setState(535);
    match(CypherParser::K_AS);
    setState(536);
    variable();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- InQueryCallStatementContext ------------------------------------------------------------------

CypherParser::InQueryCallStatementContext::InQueryCallStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::InQueryCallStatementContext::K_CALL() {
  return getToken(CypherParser::K_CALL, 0);
}

CypherParser::ExplicitProcedureInvocationContext* CypherParser::InQueryCallStatementContext::explicitProcedureInvocation() {
  return getRuleContext<CypherParser::ExplicitProcedureInvocationContext>(0);
}

tree::TerminalNode* CypherParser::InQueryCallStatementContext::K_YIELD() {
  return getToken(CypherParser::K_YIELD, 0);
}

CypherParser::YieldItemsContext* CypherParser::InQueryCallStatementContext::yieldItems() {
  return getRuleContext<CypherParser::YieldItemsContext>(0);
}


size_t CypherParser::InQueryCallStatementContext::getRuleIndex() const {
  return CypherParser::RuleInQueryCallStatement;
}


std::any CypherParser::InQueryCallStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitInQueryCallStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::InQueryCallStatementContext* CypherParser::inQueryCallStatement() {
  InQueryCallStatementContext *_localctx = _tracker.createInstance<InQueryCallStatementContext>(_ctx, getState());
  enterRule(_localctx, 44, CypherParser::RuleInQueryCallStatement);
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
    setState(538);
    match(CypherParser::K_CALL);
    setState(539);
    explicitProcedureInvocation();
    setState(542);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_YIELD) {
      setState(540);
      match(CypherParser::K_YIELD);
      setState(541);
      yieldItems();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- CreateStatementContext ------------------------------------------------------------------

CypherParser::CreateStatementContext::CreateStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::CreateStatementContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

CypherParser::PatternContext* CypherParser::CreateStatementContext::pattern() {
  return getRuleContext<CypherParser::PatternContext>(0);
}


size_t CypherParser::CreateStatementContext::getRuleIndex() const {
  return CypherParser::RuleCreateStatement;
}


std::any CypherParser::CreateStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::CreateStatementContext* CypherParser::createStatement() {
  CreateStatementContext *_localctx = _tracker.createInstance<CreateStatementContext>(_ctx, getState());
  enterRule(_localctx, 46, CypherParser::RuleCreateStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(544);
    match(CypherParser::K_CREATE);
    setState(545);
    pattern();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- MergeStatementContext ------------------------------------------------------------------

CypherParser::MergeStatementContext::MergeStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::MergeStatementContext::K_MERGE() {
  return getToken(CypherParser::K_MERGE, 0);
}

CypherParser::PatternPartContext* CypherParser::MergeStatementContext::patternPart() {
  return getRuleContext<CypherParser::PatternPartContext>(0);
}

std::vector<tree::TerminalNode *> CypherParser::MergeStatementContext::K_ON() {
  return getTokens(CypherParser::K_ON);
}

tree::TerminalNode* CypherParser::MergeStatementContext::K_ON(size_t i) {
  return getToken(CypherParser::K_ON, i);
}

std::vector<CypherParser::SetStatementContext *> CypherParser::MergeStatementContext::setStatement() {
  return getRuleContexts<CypherParser::SetStatementContext>();
}

CypherParser::SetStatementContext* CypherParser::MergeStatementContext::setStatement(size_t i) {
  return getRuleContext<CypherParser::SetStatementContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::MergeStatementContext::K_MATCH() {
  return getTokens(CypherParser::K_MATCH);
}

tree::TerminalNode* CypherParser::MergeStatementContext::K_MATCH(size_t i) {
  return getToken(CypherParser::K_MATCH, i);
}

std::vector<tree::TerminalNode *> CypherParser::MergeStatementContext::K_CREATE() {
  return getTokens(CypherParser::K_CREATE);
}

tree::TerminalNode* CypherParser::MergeStatementContext::K_CREATE(size_t i) {
  return getToken(CypherParser::K_CREATE, i);
}


size_t CypherParser::MergeStatementContext::getRuleIndex() const {
  return CypherParser::RuleMergeStatement;
}


std::any CypherParser::MergeStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitMergeStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MergeStatementContext* CypherParser::mergeStatement() {
  MergeStatementContext *_localctx = _tracker.createInstance<MergeStatementContext>(_ctx, getState());
  enterRule(_localctx, 48, CypherParser::RuleMergeStatement);
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
    setState(547);
    match(CypherParser::K_MERGE);
    setState(548);
    patternPart();
    setState(554);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_ON) {
      setState(549);
      match(CypherParser::K_ON);
      setState(550);
      _la = _input->LA(1);
      if (!(_la == CypherParser::K_CREATE

      || _la == CypherParser::K_MATCH)) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(551);
      setStatement();
      setState(556);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- SetStatementContext ------------------------------------------------------------------

CypherParser::SetStatementContext::SetStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::SetStatementContext::K_SET() {
  return getToken(CypherParser::K_SET, 0);
}

std::vector<CypherParser::SetItemContext *> CypherParser::SetStatementContext::setItem() {
  return getRuleContexts<CypherParser::SetItemContext>();
}

CypherParser::SetItemContext* CypherParser::SetStatementContext::setItem(size_t i) {
  return getRuleContext<CypherParser::SetItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::SetStatementContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::SetStatementContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::SetStatementContext::getRuleIndex() const {
  return CypherParser::RuleSetStatement;
}


std::any CypherParser::SetStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSetStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SetStatementContext* CypherParser::setStatement() {
  SetStatementContext *_localctx = _tracker.createInstance<SetStatementContext>(_ctx, getState());
  enterRule(_localctx, 50, CypherParser::RuleSetStatement);
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
    setState(557);
    match(CypherParser::K_SET);
    setState(558);
    setItem();
    setState(563);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(559);
      match(CypherParser::COMMA);
      setState(560);
      setItem();
      setState(565);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- SetItemContext ------------------------------------------------------------------

CypherParser::SetItemContext::SetItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::PropertyExpressionContext* CypherParser::SetItemContext::propertyExpression() {
  return getRuleContext<CypherParser::PropertyExpressionContext>(0);
}

tree::TerminalNode* CypherParser::SetItemContext::EQ() {
  return getToken(CypherParser::EQ, 0);
}

CypherParser::ExpressionContext* CypherParser::SetItemContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

CypherParser::VariableContext* CypherParser::SetItemContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::SetItemContext::PLUS() {
  return getToken(CypherParser::PLUS, 0);
}

CypherParser::NodeLabelsContext* CypherParser::SetItemContext::nodeLabels() {
  return getRuleContext<CypherParser::NodeLabelsContext>(0);
}


size_t CypherParser::SetItemContext::getRuleIndex() const {
  return CypherParser::RuleSetItem;
}


std::any CypherParser::SetItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSetItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SetItemContext* CypherParser::setItem() {
  SetItemContext *_localctx = _tracker.createInstance<SetItemContext>(_ctx, getState());
  enterRule(_localctx, 52, CypherParser::RuleSetItem);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(582);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 52, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(566);
      propertyExpression();
      setState(567);
      match(CypherParser::EQ);
      setState(568);
      expression();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(570);
      variable();
      setState(571);
      match(CypherParser::EQ);
      setState(572);
      expression();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(574);
      variable();
      setState(575);
      match(CypherParser::PLUS);
      setState(576);
      match(CypherParser::EQ);
      setState(577);
      expression();
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(579);
      variable();
      setState(580);
      nodeLabels();
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

//----------------- DeleteStatementContext ------------------------------------------------------------------

CypherParser::DeleteStatementContext::DeleteStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::DeleteStatementContext::K_DELETE() {
  return getToken(CypherParser::K_DELETE, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::DeleteStatementContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::DeleteStatementContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

tree::TerminalNode* CypherParser::DeleteStatementContext::K_DETACH() {
  return getToken(CypherParser::K_DETACH, 0);
}

std::vector<tree::TerminalNode *> CypherParser::DeleteStatementContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::DeleteStatementContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::DeleteStatementContext::getRuleIndex() const {
  return CypherParser::RuleDeleteStatement;
}


std::any CypherParser::DeleteStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitDeleteStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::DeleteStatementContext* CypherParser::deleteStatement() {
  DeleteStatementContext *_localctx = _tracker.createInstance<DeleteStatementContext>(_ctx, getState());
  enterRule(_localctx, 54, CypherParser::RuleDeleteStatement);
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
    setState(585);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_DETACH) {
      setState(584);
      match(CypherParser::K_DETACH);
    }
    setState(587);
    match(CypherParser::K_DELETE);
    setState(588);
    expression();
    setState(593);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(589);
      match(CypherParser::COMMA);
      setState(590);
      expression();
      setState(595);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- RemoveStatementContext ------------------------------------------------------------------

CypherParser::RemoveStatementContext::RemoveStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::RemoveStatementContext::K_REMOVE() {
  return getToken(CypherParser::K_REMOVE, 0);
}

std::vector<CypherParser::RemoveItemContext *> CypherParser::RemoveStatementContext::removeItem() {
  return getRuleContexts<CypherParser::RemoveItemContext>();
}

CypherParser::RemoveItemContext* CypherParser::RemoveStatementContext::removeItem(size_t i) {
  return getRuleContext<CypherParser::RemoveItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::RemoveStatementContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::RemoveStatementContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::RemoveStatementContext::getRuleIndex() const {
  return CypherParser::RuleRemoveStatement;
}


std::any CypherParser::RemoveStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRemoveStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RemoveStatementContext* CypherParser::removeStatement() {
  RemoveStatementContext *_localctx = _tracker.createInstance<RemoveStatementContext>(_ctx, getState());
  enterRule(_localctx, 56, CypherParser::RuleRemoveStatement);
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
    setState(596);
    match(CypherParser::K_REMOVE);
    setState(597);
    removeItem();
    setState(602);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(598);
      match(CypherParser::COMMA);
      setState(599);
      removeItem();
      setState(604);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- RemoveItemContext ------------------------------------------------------------------

CypherParser::RemoveItemContext::RemoveItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::VariableContext* CypherParser::RemoveItemContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

CypherParser::NodeLabelsContext* CypherParser::RemoveItemContext::nodeLabels() {
  return getRuleContext<CypherParser::NodeLabelsContext>(0);
}

CypherParser::PropertyExpressionContext* CypherParser::RemoveItemContext::propertyExpression() {
  return getRuleContext<CypherParser::PropertyExpressionContext>(0);
}


size_t CypherParser::RemoveItemContext::getRuleIndex() const {
  return CypherParser::RuleRemoveItem;
}


std::any CypherParser::RemoveItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRemoveItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RemoveItemContext* CypherParser::removeItem() {
  RemoveItemContext *_localctx = _tracker.createInstance<RemoveItemContext>(_ctx, getState());
  enterRule(_localctx, 58, CypherParser::RuleRemoveItem);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(609);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 56, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(605);
      variable();
      setState(606);
      nodeLabels();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(608);
      propertyExpression();
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

//----------------- ForeachStatementContext ------------------------------------------------------------------

CypherParser::ForeachStatementContext::ForeachStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ForeachStatementContext::K_FOREACH() {
  return getToken(CypherParser::K_FOREACH, 0);
}

tree::TerminalNode* CypherParser::ForeachStatementContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::VariableContext* CypherParser::ForeachStatementContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::ForeachStatementContext::K_IN() {
  return getToken(CypherParser::K_IN, 0);
}

CypherParser::ExpressionContext* CypherParser::ForeachStatementContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::ForeachStatementContext::PIPE() {
  return getToken(CypherParser::PIPE, 0);
}

tree::TerminalNode* CypherParser::ForeachStatementContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

std::vector<CypherParser::UpdatingClauseContext *> CypherParser::ForeachStatementContext::updatingClause() {
  return getRuleContexts<CypherParser::UpdatingClauseContext>();
}

CypherParser::UpdatingClauseContext* CypherParser::ForeachStatementContext::updatingClause(size_t i) {
  return getRuleContext<CypherParser::UpdatingClauseContext>(i);
}


size_t CypherParser::ForeachStatementContext::getRuleIndex() const {
  return CypherParser::RuleForeachStatement;
}


std::any CypherParser::ForeachStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitForeachStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ForeachStatementContext* CypherParser::foreachStatement() {
  ForeachStatementContext *_localctx = _tracker.createInstance<ForeachStatementContext>(_ctx, getState());
  enterRule(_localctx, 60, CypherParser::RuleForeachStatement);
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
    setState(611);
    match(CypherParser::K_FOREACH);
    setState(612);
    match(CypherParser::LPAREN);
    setState(613);
    variable();
    setState(614);
    match(CypherParser::K_IN);
    setState(615);
    expression();
    setState(616);
    match(CypherParser::PIPE);
    setState(618); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(617);
      updatingClause();
      setState(620); 
      _errHandler->sync(this);
      _la = _input->LA(1);
    } while ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 1311288) != 0) || _la == CypherParser::K_FOREACH);
    setState(622);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- CallSubqueryContext ------------------------------------------------------------------

CypherParser::CallSubqueryContext::CallSubqueryContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::CallSubqueryContext::K_CALL() {
  return getToken(CypherParser::K_CALL, 0);
}

tree::TerminalNode* CypherParser::CallSubqueryContext::LBRACE() {
  return getToken(CypherParser::LBRACE, 0);
}

CypherParser::SingleQueryContext* CypherParser::CallSubqueryContext::singleQuery() {
  return getRuleContext<CypherParser::SingleQueryContext>(0);
}

tree::TerminalNode* CypherParser::CallSubqueryContext::RBRACE() {
  return getToken(CypherParser::RBRACE, 0);
}

CypherParser::InTransactionsClauseContext* CypherParser::CallSubqueryContext::inTransactionsClause() {
  return getRuleContext<CypherParser::InTransactionsClauseContext>(0);
}


size_t CypherParser::CallSubqueryContext::getRuleIndex() const {
  return CypherParser::RuleCallSubquery;
}


std::any CypherParser::CallSubqueryContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCallSubquery(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::CallSubqueryContext* CypherParser::callSubquery() {
  CallSubqueryContext *_localctx = _tracker.createInstance<CallSubqueryContext>(_ctx, getState());
  enterRule(_localctx, 62, CypherParser::RuleCallSubquery);
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
    setState(624);
    match(CypherParser::K_CALL);
    setState(625);
    match(CypherParser::LBRACE);
    setState(626);
    singleQuery();
    setState(627);
    match(CypherParser::RBRACE);
    setState(629);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_IN) {
      setState(628);
      inTransactionsClause();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- InTransactionsClauseContext ------------------------------------------------------------------

CypherParser::InTransactionsClauseContext::InTransactionsClauseContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::InTransactionsClauseContext::K_IN() {
  return getToken(CypherParser::K_IN, 0);
}

tree::TerminalNode* CypherParser::InTransactionsClauseContext::K_TRANSACTIONS() {
  return getToken(CypherParser::K_TRANSACTIONS, 0);
}

tree::TerminalNode* CypherParser::InTransactionsClauseContext::K_OF() {
  return getToken(CypherParser::K_OF, 0);
}

CypherParser::ExpressionContext* CypherParser::InTransactionsClauseContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::InTransactionsClauseContext::K_ROWS() {
  return getToken(CypherParser::K_ROWS, 0);
}


size_t CypherParser::InTransactionsClauseContext::getRuleIndex() const {
  return CypherParser::RuleInTransactionsClause;
}


std::any CypherParser::InTransactionsClauseContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitInTransactionsClause(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::InTransactionsClauseContext* CypherParser::inTransactionsClause() {
  InTransactionsClauseContext *_localctx = _tracker.createInstance<InTransactionsClauseContext>(_ctx, getState());
  enterRule(_localctx, 64, CypherParser::RuleInTransactionsClause);
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
    setState(631);
    match(CypherParser::K_IN);
    setState(632);
    match(CypherParser::K_TRANSACTIONS);
    setState(637);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_OF) {
      setState(633);
      match(CypherParser::K_OF);
      setState(634);
      expression();
      setState(635);
      match(CypherParser::K_ROWS);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- LoadCsvStatementContext ------------------------------------------------------------------

CypherParser::LoadCsvStatementContext::LoadCsvStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::LoadCsvStatementContext::K_LOAD() {
  return getToken(CypherParser::K_LOAD, 0);
}

tree::TerminalNode* CypherParser::LoadCsvStatementContext::K_CSV() {
  return getToken(CypherParser::K_CSV, 0);
}

tree::TerminalNode* CypherParser::LoadCsvStatementContext::K_FROM() {
  return getToken(CypherParser::K_FROM, 0);
}

CypherParser::ExpressionContext* CypherParser::LoadCsvStatementContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::LoadCsvStatementContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}

CypherParser::VariableContext* CypherParser::LoadCsvStatementContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::LoadCsvStatementContext::K_WITH() {
  return getToken(CypherParser::K_WITH, 0);
}

tree::TerminalNode* CypherParser::LoadCsvStatementContext::K_HEADERS() {
  return getToken(CypherParser::K_HEADERS, 0);
}

tree::TerminalNode* CypherParser::LoadCsvStatementContext::K_FIELDTERMINATOR() {
  return getToken(CypherParser::K_FIELDTERMINATOR, 0);
}

tree::TerminalNode* CypherParser::LoadCsvStatementContext::StringLiteral() {
  return getToken(CypherParser::StringLiteral, 0);
}


size_t CypherParser::LoadCsvStatementContext::getRuleIndex() const {
  return CypherParser::RuleLoadCsvStatement;
}


std::any CypherParser::LoadCsvStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitLoadCsvStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::LoadCsvStatementContext* CypherParser::loadCsvStatement() {
  LoadCsvStatementContext *_localctx = _tracker.createInstance<LoadCsvStatementContext>(_ctx, getState());
  enterRule(_localctx, 66, CypherParser::RuleLoadCsvStatement);
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
    setState(639);
    match(CypherParser::K_LOAD);
    setState(640);
    match(CypherParser::K_CSV);
    setState(643);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WITH) {
      setState(641);
      match(CypherParser::K_WITH);
      setState(642);
      match(CypherParser::K_HEADERS);
    }
    setState(645);
    match(CypherParser::K_FROM);
    setState(646);
    expression();
    setState(647);
    match(CypherParser::K_AS);
    setState(648);
    variable();
    setState(651);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_FIELDTERMINATOR) {
      setState(649);
      match(CypherParser::K_FIELDTERMINATOR);
      setState(650);
      match(CypherParser::StringLiteral);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ReturnStatementContext ------------------------------------------------------------------

CypherParser::ReturnStatementContext::ReturnStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ReturnStatementContext::K_RETURN() {
  return getToken(CypherParser::K_RETURN, 0);
}

CypherParser::ProjectionBodyContext* CypherParser::ReturnStatementContext::projectionBody() {
  return getRuleContext<CypherParser::ProjectionBodyContext>(0);
}


size_t CypherParser::ReturnStatementContext::getRuleIndex() const {
  return CypherParser::RuleReturnStatement;
}


std::any CypherParser::ReturnStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitReturnStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ReturnStatementContext* CypherParser::returnStatement() {
  ReturnStatementContext *_localctx = _tracker.createInstance<ReturnStatementContext>(_ctx, getState());
  enterRule(_localctx, 68, CypherParser::RuleReturnStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(653);
    match(CypherParser::K_RETURN);
    setState(654);
    projectionBody();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- StandaloneCallStatementContext ------------------------------------------------------------------

CypherParser::StandaloneCallStatementContext::StandaloneCallStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::StandaloneCallStatementContext::K_CALL() {
  return getToken(CypherParser::K_CALL, 0);
}

CypherParser::ExplicitProcedureInvocationContext* CypherParser::StandaloneCallStatementContext::explicitProcedureInvocation() {
  return getRuleContext<CypherParser::ExplicitProcedureInvocationContext>(0);
}

CypherParser::ImplicitProcedureInvocationContext* CypherParser::StandaloneCallStatementContext::implicitProcedureInvocation() {
  return getRuleContext<CypherParser::ImplicitProcedureInvocationContext>(0);
}

tree::TerminalNode* CypherParser::StandaloneCallStatementContext::K_YIELD() {
  return getToken(CypherParser::K_YIELD, 0);
}

tree::TerminalNode* CypherParser::StandaloneCallStatementContext::MULTIPLY() {
  return getToken(CypherParser::MULTIPLY, 0);
}

CypherParser::YieldItemsContext* CypherParser::StandaloneCallStatementContext::yieldItems() {
  return getRuleContext<CypherParser::YieldItemsContext>(0);
}


size_t CypherParser::StandaloneCallStatementContext::getRuleIndex() const {
  return CypherParser::RuleStandaloneCallStatement;
}


std::any CypherParser::StandaloneCallStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitStandaloneCallStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::StandaloneCallStatementContext* CypherParser::standaloneCallStatement() {
  StandaloneCallStatementContext *_localctx = _tracker.createInstance<StandaloneCallStatementContext>(_ctx, getState());
  enterRule(_localctx, 70, CypherParser::RuleStandaloneCallStatement);
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
    setState(656);
    match(CypherParser::K_CALL);
    setState(659);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 62, _ctx)) {
    case 1: {
      setState(657);
      explicitProcedureInvocation();
      break;
    }

    case 2: {
      setState(658);
      implicitProcedureInvocation();
      break;
    }

    default:
      break;
    }
    setState(666);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_YIELD) {
      setState(661);
      match(CypherParser::K_YIELD);
      setState(664);
      _errHandler->sync(this);
      switch (_input->LA(1)) {
        case CypherParser::MULTIPLY: {
          setState(662);
          match(CypherParser::MULTIPLY);
          break;
        }

        case CypherParser::K_CALL:
        case CypherParser::K_YIELD:
        case CypherParser::K_CREATE:
        case CypherParser::K_DELETE:
        case CypherParser::K_DETACH:
        case CypherParser::K_EXISTS:
        case CypherParser::K_LIMIT:
        case CypherParser::K_MATCH:
        case CypherParser::K_MERGE:
        case CypherParser::K_OPTIONAL:
        case CypherParser::K_ORDER:
        case CypherParser::K_BY:
        case CypherParser::K_SKIP:
        case CypherParser::K_ASCENDING:
        case CypherParser::K_ASC:
        case CypherParser::K_DESCENDING:
        case CypherParser::K_DESC:
        case CypherParser::K_REMOVE:
        case CypherParser::K_RETURN:
        case CypherParser::K_SET:
        case CypherParser::K_WHERE:
        case CypherParser::K_WITH:
        case CypherParser::K_UNION:
        case CypherParser::K_UNWIND:
        case CypherParser::K_AND:
        case CypherParser::K_AS:
        case CypherParser::K_CONTAINS:
        case CypherParser::K_DISTINCT:
        case CypherParser::K_ENDS:
        case CypherParser::K_IN:
        case CypherParser::K_IS:
        case CypherParser::K_NOT:
        case CypherParser::K_OR:
        case CypherParser::K_STARTS:
        case CypherParser::K_XOR:
        case CypherParser::K_BETWEEN:
        case CypherParser::K_FALSE:
        case CypherParser::K_TRUE:
        case CypherParser::K_NULL:
        case CypherParser::K_CASE:
        case CypherParser::K_WHEN:
        case CypherParser::K_THEN:
        case CypherParser::K_ELSE:
        case CypherParser::K_END:
        case CypherParser::K_COUNT:
        case CypherParser::K_FILTER:
        case CypherParser::K_EXTRACT:
        case CypherParser::K_ANY:
        case CypherParser::K_NONE:
        case CypherParser::K_SINGLE:
        case CypherParser::K_ALL:
        case CypherParser::K_REDUCE:
        case CypherParser::K_SHORTESTPATH:
        case CypherParser::K_ALLSHORTESTPATHS:
        case CypherParser::K_INDEX:
        case CypherParser::K_ON:
        case CypherParser::K_SHOW:
        case CypherParser::K_DROP:
        case CypherParser::K_IF:
        case CypherParser::K_FOR:
        case CypherParser::K_CONSTRAINT:
        case CypherParser::K_DO:
        case CypherParser::K_REQUIRE:
        case CypherParser::K_UNIQUE:
        case CypherParser::K_MANDATORY:
        case CypherParser::K_SCALAR:
        case CypherParser::K_OF:
        case CypherParser::K_ADD:
        case CypherParser::K_BEGIN:
        case CypherParser::K_COMMIT:
        case CypherParser::K_ROLLBACK:
        case CypherParser::K_TRANSACTION:
        case CypherParser::K_KEY:
        case CypherParser::K_NODE:
        case CypherParser::K_BOOLEAN:
        case CypherParser::K_INTEGER:
        case CypherParser::K_FLOAT:
        case CypherParser::K_STRING:
        case CypherParser::K_LIST:
        case CypherParser::K_MAP:
        case CypherParser::K_VECTOR:
        case CypherParser::K_OPTIONS:
        case CypherParser::K_FOREACH:
        case CypherParser::K_LOAD:
        case CypherParser::K_CSV:
        case CypherParser::K_HEADERS:
        case CypherParser::K_FROM:
        case CypherParser::K_FIELDTERMINATOR:
        case CypherParser::K_TRANSACTIONS:
        case CypherParser::K_ROWS:
        case CypherParser::K_EXPLAIN:
        case CypherParser::K_PROFILE:
        case CypherParser::ID: {
          setState(663);
          yieldItems();
          break;
        }

      default:
        throw NoViableAltException(this);
      }
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- YieldItemsContext ------------------------------------------------------------------

CypherParser::YieldItemsContext::YieldItemsContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::YieldItemContext *> CypherParser::YieldItemsContext::yieldItem() {
  return getRuleContexts<CypherParser::YieldItemContext>();
}

CypherParser::YieldItemContext* CypherParser::YieldItemsContext::yieldItem(size_t i) {
  return getRuleContext<CypherParser::YieldItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::YieldItemsContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::YieldItemsContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}

tree::TerminalNode* CypherParser::YieldItemsContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

CypherParser::WhereContext* CypherParser::YieldItemsContext::where() {
  return getRuleContext<CypherParser::WhereContext>(0);
}


size_t CypherParser::YieldItemsContext::getRuleIndex() const {
  return CypherParser::RuleYieldItems;
}


std::any CypherParser::YieldItemsContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitYieldItems(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::YieldItemsContext* CypherParser::yieldItems() {
  YieldItemsContext *_localctx = _tracker.createInstance<YieldItemsContext>(_ctx, getState());
  enterRule(_localctx, 72, CypherParser::RuleYieldItems);
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
    setState(668);
    yieldItem();
    setState(673);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(669);
      match(CypherParser::COMMA);
      setState(670);
      yieldItem();
      setState(675);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
    setState(678);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(676);
      match(CypherParser::K_WHERE);
      setState(677);
      where();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- YieldItemContext ------------------------------------------------------------------

CypherParser::YieldItemContext::YieldItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::VariableContext* CypherParser::YieldItemContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

CypherParser::ProcedureResultFieldContext* CypherParser::YieldItemContext::procedureResultField() {
  return getRuleContext<CypherParser::ProcedureResultFieldContext>(0);
}

tree::TerminalNode* CypherParser::YieldItemContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}


size_t CypherParser::YieldItemContext::getRuleIndex() const {
  return CypherParser::RuleYieldItem;
}


std::any CypherParser::YieldItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitYieldItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::YieldItemContext* CypherParser::yieldItem() {
  YieldItemContext *_localctx = _tracker.createInstance<YieldItemContext>(_ctx, getState());
  enterRule(_localctx, 74, CypherParser::RuleYieldItem);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(683);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 67, _ctx)) {
    case 1: {
      setState(680);
      procedureResultField();
      setState(681);
      match(CypherParser::K_AS);
      break;
    }

    default:
      break;
    }
    setState(685);
    variable();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

