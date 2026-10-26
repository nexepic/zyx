//----------------- ProjectionBodyContext ------------------------------------------------------------------

CypherParser::ProjectionBodyContext::ProjectionBodyContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ProjectionItemsContext* CypherParser::ProjectionBodyContext::projectionItems() {
  return getRuleContext<CypherParser::ProjectionItemsContext>(0);
}

tree::TerminalNode* CypherParser::ProjectionBodyContext::K_DISTINCT() {
  return getToken(CypherParser::K_DISTINCT, 0);
}

CypherParser::OrderStatementContext* CypherParser::ProjectionBodyContext::orderStatement() {
  return getRuleContext<CypherParser::OrderStatementContext>(0);
}

CypherParser::SkipStatementContext* CypherParser::ProjectionBodyContext::skipStatement() {
  return getRuleContext<CypherParser::SkipStatementContext>(0);
}

CypherParser::LimitStatementContext* CypherParser::ProjectionBodyContext::limitStatement() {
  return getRuleContext<CypherParser::LimitStatementContext>(0);
}


size_t CypherParser::ProjectionBodyContext::getRuleIndex() const {
  return CypherParser::RuleProjectionBody;
}


std::any CypherParser::ProjectionBodyContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProjectionBody(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProjectionBodyContext* CypherParser::projectionBody() {
  ProjectionBodyContext *_localctx = _tracker.createInstance<ProjectionBodyContext>(_ctx, getState());
  enterRule(_localctx, 76, CypherParser::RuleProjectionBody);
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
    setState(688);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 68, _ctx)) {
    case 1: {
      setState(687);
      match(CypherParser::K_DISTINCT);
      break;
    }

    default:
      break;
    }
    setState(690);
    projectionItems();
    setState(692);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_ORDER) {
      setState(691);
      orderStatement();
    }
    setState(695);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_SKIP) {
      setState(694);
      skipStatement();
    }
    setState(698);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_LIMIT) {
      setState(697);
      limitStatement();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ProjectionItemsContext ------------------------------------------------------------------

CypherParser::ProjectionItemsContext::ProjectionItemsContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ProjectionItemsContext::MULTIPLY() {
  return getToken(CypherParser::MULTIPLY, 0);
}

std::vector<CypherParser::ProjectionItemContext *> CypherParser::ProjectionItemsContext::projectionItem() {
  return getRuleContexts<CypherParser::ProjectionItemContext>();
}

CypherParser::ProjectionItemContext* CypherParser::ProjectionItemsContext::projectionItem(size_t i) {
  return getRuleContext<CypherParser::ProjectionItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ProjectionItemsContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::ProjectionItemsContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::ProjectionItemsContext::getRuleIndex() const {
  return CypherParser::RuleProjectionItems;
}


std::any CypherParser::ProjectionItemsContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProjectionItems(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProjectionItemsContext* CypherParser::projectionItems() {
  ProjectionItemsContext *_localctx = _tracker.createInstance<ProjectionItemsContext>(_ctx, getState());
  enterRule(_localctx, 78, CypherParser::RuleProjectionItems);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(709);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::MULTIPLY: {
        enterOuterAlt(_localctx, 1);
        setState(700);
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
      case CypherParser::PLUS:
      case CypherParser::MINUS:
      case CypherParser::LPAREN:
      case CypherParser::LBRACE:
      case CypherParser::LBRACK:
      case CypherParser::DOLLAR:
      case CypherParser::HexInteger:
      case CypherParser::OctalInteger:
      case CypherParser::DecimalInteger:
      case CypherParser::DoubleLiteral:
      case CypherParser::ID:
      case CypherParser::StringLiteral: {
        enterOuterAlt(_localctx, 2);
        setState(701);
        projectionItem();
        setState(706);
        _errHandler->sync(this);
        _la = _input->LA(1);
        while (_la == CypherParser::COMMA) {
          setState(702);
          match(CypherParser::COMMA);
          setState(703);
          projectionItem();
          setState(708);
          _errHandler->sync(this);
          _la = _input->LA(1);
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

//----------------- ProjectionItemContext ------------------------------------------------------------------

CypherParser::ProjectionItemContext::ProjectionItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ExpressionContext* CypherParser::ProjectionItemContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::ProjectionItemContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}

CypherParser::VariableContext* CypherParser::ProjectionItemContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}


size_t CypherParser::ProjectionItemContext::getRuleIndex() const {
  return CypherParser::RuleProjectionItem;
}


std::any CypherParser::ProjectionItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProjectionItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProjectionItemContext* CypherParser::projectionItem() {
  ProjectionItemContext *_localctx = _tracker.createInstance<ProjectionItemContext>(_ctx, getState());
  enterRule(_localctx, 80, CypherParser::RuleProjectionItem);
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
    setState(711);
    expression();
    setState(714);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_AS) {
      setState(712);
      match(CypherParser::K_AS);
      setState(713);
      variable();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- OrderStatementContext ------------------------------------------------------------------

CypherParser::OrderStatementContext::OrderStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::OrderStatementContext::K_ORDER() {
  return getToken(CypherParser::K_ORDER, 0);
}

tree::TerminalNode* CypherParser::OrderStatementContext::K_BY() {
  return getToken(CypherParser::K_BY, 0);
}

std::vector<CypherParser::SortItemContext *> CypherParser::OrderStatementContext::sortItem() {
  return getRuleContexts<CypherParser::SortItemContext>();
}

CypherParser::SortItemContext* CypherParser::OrderStatementContext::sortItem(size_t i) {
  return getRuleContext<CypherParser::SortItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::OrderStatementContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::OrderStatementContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::OrderStatementContext::getRuleIndex() const {
  return CypherParser::RuleOrderStatement;
}


std::any CypherParser::OrderStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitOrderStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::OrderStatementContext* CypherParser::orderStatement() {
  OrderStatementContext *_localctx = _tracker.createInstance<OrderStatementContext>(_ctx, getState());
  enterRule(_localctx, 82, CypherParser::RuleOrderStatement);
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
    setState(716);
    match(CypherParser::K_ORDER);
    setState(717);
    match(CypherParser::K_BY);
    setState(718);
    sortItem();
    setState(723);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(719);
      match(CypherParser::COMMA);
      setState(720);
      sortItem();
      setState(725);
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

//----------------- SkipStatementContext ------------------------------------------------------------------

CypherParser::SkipStatementContext::SkipStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::SkipStatementContext::K_SKIP() {
  return getToken(CypherParser::K_SKIP, 0);
}

CypherParser::ExpressionContext* CypherParser::SkipStatementContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}


size_t CypherParser::SkipStatementContext::getRuleIndex() const {
  return CypherParser::RuleSkipStatement;
}


std::any CypherParser::SkipStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSkipStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SkipStatementContext* CypherParser::skipStatement() {
  SkipStatementContext *_localctx = _tracker.createInstance<SkipStatementContext>(_ctx, getState());
  enterRule(_localctx, 84, CypherParser::RuleSkipStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(726);
    match(CypherParser::K_SKIP);
    setState(727);
    expression();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- LimitStatementContext ------------------------------------------------------------------

CypherParser::LimitStatementContext::LimitStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::LimitStatementContext::K_LIMIT() {
  return getToken(CypherParser::K_LIMIT, 0);
}

CypherParser::ExpressionContext* CypherParser::LimitStatementContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}


size_t CypherParser::LimitStatementContext::getRuleIndex() const {
  return CypherParser::RuleLimitStatement;
}


std::any CypherParser::LimitStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitLimitStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::LimitStatementContext* CypherParser::limitStatement() {
  LimitStatementContext *_localctx = _tracker.createInstance<LimitStatementContext>(_ctx, getState());
  enterRule(_localctx, 86, CypherParser::RuleLimitStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(729);
    match(CypherParser::K_LIMIT);
    setState(730);
    expression();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- SortItemContext ------------------------------------------------------------------

CypherParser::SortItemContext::SortItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ExpressionContext* CypherParser::SortItemContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::SortItemContext::K_ASCENDING() {
  return getToken(CypherParser::K_ASCENDING, 0);
}

tree::TerminalNode* CypherParser::SortItemContext::K_ASC() {
  return getToken(CypherParser::K_ASC, 0);
}

tree::TerminalNode* CypherParser::SortItemContext::K_DESCENDING() {
  return getToken(CypherParser::K_DESCENDING, 0);
}

tree::TerminalNode* CypherParser::SortItemContext::K_DESC() {
  return getToken(CypherParser::K_DESC, 0);
}


size_t CypherParser::SortItemContext::getRuleIndex() const {
  return CypherParser::RuleSortItem;
}


std::any CypherParser::SortItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSortItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SortItemContext* CypherParser::sortItem() {
  SortItemContext *_localctx = _tracker.createInstance<SortItemContext>(_ctx, getState());
  enterRule(_localctx, 88, CypherParser::RuleSortItem);
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
    setState(732);
    expression();
    setState(734);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 245760) != 0)) {
      setState(733);
      _la = _input->LA(1);
      if (!((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 245760) != 0))) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
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

//----------------- WhereContext ------------------------------------------------------------------

CypherParser::WhereContext::WhereContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ExpressionContext* CypherParser::WhereContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}


size_t CypherParser::WhereContext::getRuleIndex() const {
  return CypherParser::RuleWhere;
}


std::any CypherParser::WhereContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitWhere(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::WhereContext* CypherParser::where() {
  WhereContext *_localctx = _tracker.createInstance<WhereContext>(_ctx, getState());
  enterRule(_localctx, 90, CypherParser::RuleWhere);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(736);
    expression();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PatternContext ------------------------------------------------------------------

CypherParser::PatternContext::PatternContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::PatternPartContext *> CypherParser::PatternContext::patternPart() {
  return getRuleContexts<CypherParser::PatternPartContext>();
}

CypherParser::PatternPartContext* CypherParser::PatternContext::patternPart(size_t i) {
  return getRuleContext<CypherParser::PatternPartContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::PatternContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::PatternContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::PatternContext::getRuleIndex() const {
  return CypherParser::RulePattern;
}


std::any CypherParser::PatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternContext* CypherParser::pattern() {
  PatternContext *_localctx = _tracker.createInstance<PatternContext>(_ctx, getState());
  enterRule(_localctx, 92, CypherParser::RulePattern);
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
    setState(738);
    patternPart();
    setState(743);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(739);
      match(CypherParser::COMMA);
      setState(740);
      patternPart();
      setState(745);
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

//----------------- PatternPartContext ------------------------------------------------------------------

CypherParser::PatternPartContext::PatternPartContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::PatternElementContext* CypherParser::PatternPartContext::patternElement() {
  return getRuleContext<CypherParser::PatternElementContext>(0);
}

CypherParser::VariableContext* CypherParser::PatternPartContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::PatternPartContext::EQ() {
  return getToken(CypherParser::EQ, 0);
}


size_t CypherParser::PatternPartContext::getRuleIndex() const {
  return CypherParser::RulePatternPart;
}


std::any CypherParser::PatternPartContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPatternPart(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternPartContext* CypherParser::patternPart() {
  PatternPartContext *_localctx = _tracker.createInstance<PatternPartContext>(_ctx, getState());
  enterRule(_localctx, 94, CypherParser::RulePatternPart);
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
    setState(749);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 576460752840294399) != 0)) {
      setState(746);
      variable();
      setState(747);
      match(CypherParser::EQ);
    }
    setState(751);
    patternElement();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PatternElementContext ------------------------------------------------------------------

CypherParser::PatternElementContext::PatternElementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::NodePatternContext* CypherParser::PatternElementContext::nodePattern() {
  return getRuleContext<CypherParser::NodePatternContext>(0);
}

std::vector<CypherParser::PatternElementChainContext *> CypherParser::PatternElementContext::patternElementChain() {
  return getRuleContexts<CypherParser::PatternElementChainContext>();
}

CypherParser::PatternElementChainContext* CypherParser::PatternElementContext::patternElementChain(size_t i) {
  return getRuleContext<CypherParser::PatternElementChainContext>(i);
}

tree::TerminalNode* CypherParser::PatternElementContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PatternElementContext* CypherParser::PatternElementContext::patternElement() {
  return getRuleContext<CypherParser::PatternElementContext>(0);
}

tree::TerminalNode* CypherParser::PatternElementContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}


size_t CypherParser::PatternElementContext::getRuleIndex() const {
  return CypherParser::RulePatternElement;
}


std::any CypherParser::PatternElementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPatternElement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternElementContext* CypherParser::patternElement() {
  PatternElementContext *_localctx = _tracker.createInstance<PatternElementContext>(_ctx, getState());
  enterRule(_localctx, 96, CypherParser::RulePatternElement);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(764);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 80, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(753);
      nodePattern();
      setState(757);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::LT

      || _la == CypherParser::MINUS) {
        setState(754);
        patternElementChain();
        setState(759);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(760);
      match(CypherParser::LPAREN);
      setState(761);
      patternElement();
      setState(762);
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

//----------------- PatternElementChainContext ------------------------------------------------------------------

CypherParser::PatternElementChainContext::PatternElementChainContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::RelationshipPatternContext* CypherParser::PatternElementChainContext::relationshipPattern() {
  return getRuleContext<CypherParser::RelationshipPatternContext>(0);
}

CypherParser::NodePatternContext* CypherParser::PatternElementChainContext::nodePattern() {
  return getRuleContext<CypherParser::NodePatternContext>(0);
}


size_t CypherParser::PatternElementChainContext::getRuleIndex() const {
  return CypherParser::RulePatternElementChain;
}


std::any CypherParser::PatternElementChainContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPatternElementChain(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternElementChainContext* CypherParser::patternElementChain() {
  PatternElementChainContext *_localctx = _tracker.createInstance<PatternElementChainContext>(_ctx, getState());
  enterRule(_localctx, 98, CypherParser::RulePatternElementChain);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(766);
    relationshipPattern();
    setState(767);
    nodePattern();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- NodePatternContext ------------------------------------------------------------------

CypherParser::NodePatternContext::NodePatternContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::NodePatternContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::NodePatternContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::VariableContext* CypherParser::NodePatternContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

CypherParser::NodeLabelsContext* CypherParser::NodePatternContext::nodeLabels() {
  return getRuleContext<CypherParser::NodeLabelsContext>(0);
}

CypherParser::PropertiesContext* CypherParser::NodePatternContext::properties() {
  return getRuleContext<CypherParser::PropertiesContext>(0);
}


size_t CypherParser::NodePatternContext::getRuleIndex() const {
  return CypherParser::RuleNodePattern;
}


std::any CypherParser::NodePatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNodePattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NodePatternContext* CypherParser::nodePattern() {
  NodePatternContext *_localctx = _tracker.createInstance<NodePatternContext>(_ctx, getState());
  enterRule(_localctx, 100, CypherParser::RuleNodePattern);
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
    setState(769);
    match(CypherParser::LPAREN);
    setState(771);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 576460752840294399) != 0)) {
      setState(770);
      variable();
    }
    setState(774);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(773);
      nodeLabels();
    }
    setState(777);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE

    || _la == CypherParser::DOLLAR) {
      setState(776);
      properties();
    }
    setState(779);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- RelationshipPatternContext ------------------------------------------------------------------

CypherParser::RelationshipPatternContext::RelationshipPatternContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<tree::TerminalNode *> CypherParser::RelationshipPatternContext::MINUS() {
  return getTokens(CypherParser::MINUS);
}

tree::TerminalNode* CypherParser::RelationshipPatternContext::MINUS(size_t i) {
  return getToken(CypherParser::MINUS, i);
}

tree::TerminalNode* CypherParser::RelationshipPatternContext::LT() {
  return getToken(CypherParser::LT, 0);
}

CypherParser::RelationshipDetailContext* CypherParser::RelationshipPatternContext::relationshipDetail() {
  return getRuleContext<CypherParser::RelationshipDetailContext>(0);
}

tree::TerminalNode* CypherParser::RelationshipPatternContext::GT() {
  return getToken(CypherParser::GT, 0);
}


size_t CypherParser::RelationshipPatternContext::getRuleIndex() const {
  return CypherParser::RuleRelationshipPattern;
}


std::any CypherParser::RelationshipPatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRelationshipPattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RelationshipPatternContext* CypherParser::relationshipPattern() {
  RelationshipPatternContext *_localctx = _tracker.createInstance<RelationshipPatternContext>(_ctx, getState());
  enterRule(_localctx, 102, CypherParser::RuleRelationshipPattern);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(797);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 88, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(782);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LT) {
        setState(781);
        match(CypherParser::LT);
      }
      setState(784);
      match(CypherParser::MINUS);
      setState(786);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LBRACK) {
        setState(785);
        relationshipDetail();
      }
      setState(788);
      match(CypherParser::MINUS);
      setState(790);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::GT) {
        setState(789);
        match(CypherParser::GT);
      }
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(792);
      match(CypherParser::MINUS);
      setState(794);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LBRACK) {
        setState(793);
        relationshipDetail();
      }
      setState(796);
      match(CypherParser::MINUS);
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

//----------------- RelationshipDetailContext ------------------------------------------------------------------

CypherParser::RelationshipDetailContext::RelationshipDetailContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::RelationshipDetailContext::LBRACK() {
  return getToken(CypherParser::LBRACK, 0);
}

tree::TerminalNode* CypherParser::RelationshipDetailContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}

CypherParser::VariableContext* CypherParser::RelationshipDetailContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

CypherParser::RelationshipTypesContext* CypherParser::RelationshipDetailContext::relationshipTypes() {
  return getRuleContext<CypherParser::RelationshipTypesContext>(0);
}

CypherParser::RangeLiteralContext* CypherParser::RelationshipDetailContext::rangeLiteral() {
  return getRuleContext<CypherParser::RangeLiteralContext>(0);
}

CypherParser::PropertiesContext* CypherParser::RelationshipDetailContext::properties() {
  return getRuleContext<CypherParser::PropertiesContext>(0);
}


size_t CypherParser::RelationshipDetailContext::getRuleIndex() const {
  return CypherParser::RuleRelationshipDetail;
}


std::any CypherParser::RelationshipDetailContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRelationshipDetail(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RelationshipDetailContext* CypherParser::relationshipDetail() {
  RelationshipDetailContext *_localctx = _tracker.createInstance<RelationshipDetailContext>(_ctx, getState());
  enterRule(_localctx, 104, CypherParser::RuleRelationshipDetail);
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
    setState(799);
    match(CypherParser::LBRACK);
    setState(801);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 576460752840294399) != 0)) {
      setState(800);
      variable();
    }
    setState(804);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(803);
      relationshipTypes();
    }
    setState(807);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::MULTIPLY) {
      setState(806);
      rangeLiteral();
    }
    setState(810);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE

    || _la == CypherParser::DOLLAR) {
      setState(809);
      properties();
    }
    setState(812);
    match(CypherParser::RBRACK);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PropertiesContext ------------------------------------------------------------------

CypherParser::PropertiesContext::PropertiesContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::MapLiteralContext* CypherParser::PropertiesContext::mapLiteral() {
  return getRuleContext<CypherParser::MapLiteralContext>(0);
}

CypherParser::ParameterContext* CypherParser::PropertiesContext::parameter() {
  return getRuleContext<CypherParser::ParameterContext>(0);
}


size_t CypherParser::PropertiesContext::getRuleIndex() const {
  return CypherParser::RuleProperties;
}


std::any CypherParser::PropertiesContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProperties(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PropertiesContext* CypherParser::properties() {
  PropertiesContext *_localctx = _tracker.createInstance<PropertiesContext>(_ctx, getState());
  enterRule(_localctx, 106, CypherParser::RuleProperties);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(816);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::LBRACE: {
        enterOuterAlt(_localctx, 1);
        setState(814);
        mapLiteral();
        break;
      }

      case CypherParser::DOLLAR: {
        enterOuterAlt(_localctx, 2);
        setState(815);
        parameter();
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

//----------------- NodeLabelsContext ------------------------------------------------------------------

CypherParser::NodeLabelsContext::NodeLabelsContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::NodeLabelContext *> CypherParser::NodeLabelsContext::nodeLabel() {
  return getRuleContexts<CypherParser::NodeLabelContext>();
}

CypherParser::NodeLabelContext* CypherParser::NodeLabelsContext::nodeLabel(size_t i) {
  return getRuleContext<CypherParser::NodeLabelContext>(i);
}


size_t CypherParser::NodeLabelsContext::getRuleIndex() const {
  return CypherParser::RuleNodeLabels;
}


std::any CypherParser::NodeLabelsContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNodeLabels(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NodeLabelsContext* CypherParser::nodeLabels() {
  NodeLabelsContext *_localctx = _tracker.createInstance<NodeLabelsContext>(_ctx, getState());
  enterRule(_localctx, 108, CypherParser::RuleNodeLabels);
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
    setState(819); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(818);
      nodeLabel();
      setState(821); 
      _errHandler->sync(this);
      _la = _input->LA(1);
    } while (_la == CypherParser::COLON);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- NodeLabelContext ------------------------------------------------------------------

CypherParser::NodeLabelContext::NodeLabelContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::NodeLabelContext::COLON() {
  return getToken(CypherParser::COLON, 0);
}

CypherParser::LabelNameContext* CypherParser::NodeLabelContext::labelName() {
  return getRuleContext<CypherParser::LabelNameContext>(0);
}


size_t CypherParser::NodeLabelContext::getRuleIndex() const {
  return CypherParser::RuleNodeLabel;
}


std::any CypherParser::NodeLabelContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNodeLabel(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NodeLabelContext* CypherParser::nodeLabel() {
  NodeLabelContext *_localctx = _tracker.createInstance<NodeLabelContext>(_ctx, getState());
  enterRule(_localctx, 110, CypherParser::RuleNodeLabel);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(823);
    match(CypherParser::COLON);
    setState(824);
    labelName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- RelationshipTypesContext ------------------------------------------------------------------

CypherParser::RelationshipTypesContext::RelationshipTypesContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<tree::TerminalNode *> CypherParser::RelationshipTypesContext::COLON() {
  return getTokens(CypherParser::COLON);
}

tree::TerminalNode* CypherParser::RelationshipTypesContext::COLON(size_t i) {
  return getToken(CypherParser::COLON, i);
}

std::vector<CypherParser::RelTypeNameContext *> CypherParser::RelationshipTypesContext::relTypeName() {
  return getRuleContexts<CypherParser::RelTypeNameContext>();
}

CypherParser::RelTypeNameContext* CypherParser::RelationshipTypesContext::relTypeName(size_t i) {
  return getRuleContext<CypherParser::RelTypeNameContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::RelationshipTypesContext::PIPE() {
  return getTokens(CypherParser::PIPE);
}

tree::TerminalNode* CypherParser::RelationshipTypesContext::PIPE(size_t i) {
  return getToken(CypherParser::PIPE, i);
}


size_t CypherParser::RelationshipTypesContext::getRuleIndex() const {
  return CypherParser::RuleRelationshipTypes;
}


std::any CypherParser::RelationshipTypesContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRelationshipTypes(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RelationshipTypesContext* CypherParser::relationshipTypes() {
  RelationshipTypesContext *_localctx = _tracker.createInstance<RelationshipTypesContext>(_ctx, getState());
  enterRule(_localctx, 112, CypherParser::RuleRelationshipTypes);
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
    setState(826);
    match(CypherParser::COLON);
    setState(827);
    relTypeName();
    setState(835);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::PIPE) {
      setState(828);
      match(CypherParser::PIPE);
      setState(830);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::COLON) {
        setState(829);
        match(CypherParser::COLON);
      }
      setState(832);
      relTypeName();
      setState(837);
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

//----------------- RangeLiteralContext ------------------------------------------------------------------

CypherParser::RangeLiteralContext::RangeLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::RangeLiteralContext::MULTIPLY() {
  return getToken(CypherParser::MULTIPLY, 0);
}

std::vector<CypherParser::IntegerLiteralContext *> CypherParser::RangeLiteralContext::integerLiteral() {
  return getRuleContexts<CypherParser::IntegerLiteralContext>();
}

CypherParser::IntegerLiteralContext* CypherParser::RangeLiteralContext::integerLiteral(size_t i) {
  return getRuleContext<CypherParser::IntegerLiteralContext>(i);
}

tree::TerminalNode* CypherParser::RangeLiteralContext::RANGE() {
  return getToken(CypherParser::RANGE, 0);
}


size_t CypherParser::RangeLiteralContext::getRuleIndex() const {
  return CypherParser::RuleRangeLiteral;
}


std::any CypherParser::RangeLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRangeLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RangeLiteralContext* CypherParser::rangeLiteral() {
  RangeLiteralContext *_localctx = _tracker.createInstance<RangeLiteralContext>(_ctx, getState());
  enterRule(_localctx, 114, CypherParser::RuleRangeLiteral);
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
    setState(838);
    match(CypherParser::MULTIPLY);
    setState(840);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (((((_la - 119) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 119)) & 7) != 0)) {
      setState(839);
      integerLiteral();
    }
    setState(846);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::RANGE) {
      setState(842);
      match(CypherParser::RANGE);
      setState(844);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (((((_la - 119) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 119)) & 7) != 0)) {
        setState(843);
        integerLiteral();
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

