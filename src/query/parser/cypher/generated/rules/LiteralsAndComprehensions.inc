//----------------- LiteralContext ------------------------------------------------------------------

CypherParser::LiteralContext::LiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::BooleanLiteralContext* CypherParser::LiteralContext::booleanLiteral() {
  return getRuleContext<CypherParser::BooleanLiteralContext>(0);
}

tree::TerminalNode* CypherParser::LiteralContext::K_NULL() {
  return getToken(CypherParser::K_NULL, 0);
}

CypherParser::NumberLiteralContext* CypherParser::LiteralContext::numberLiteral() {
  return getRuleContext<CypherParser::NumberLiteralContext>(0);
}

tree::TerminalNode* CypherParser::LiteralContext::StringLiteral() {
  return getToken(CypherParser::StringLiteral, 0);
}

CypherParser::MapLiteralContext* CypherParser::LiteralContext::mapLiteral() {
  return getRuleContext<CypherParser::MapLiteralContext>(0);
}


size_t CypherParser::LiteralContext::getRuleIndex() const {
  return CypherParser::RuleLiteral;
}


std::any CypherParser::LiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::LiteralContext* CypherParser::literal() {
  LiteralContext *_localctx = _tracker.createInstance<LiteralContext>(_ctx, getState());
  enterRule(_localctx, 172, CypherParser::RuleLiteral);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(1120);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_FALSE:
      case CypherParser::K_TRUE: {
        enterOuterAlt(_localctx, 1);
        setState(1115);
        booleanLiteral();
        break;
      }

      case CypherParser::K_NULL: {
        enterOuterAlt(_localctx, 2);
        setState(1116);
        match(CypherParser::K_NULL);
        break;
      }

      case CypherParser::HexInteger:
      case CypherParser::OctalInteger:
      case CypherParser::DecimalInteger:
      case CypherParser::DoubleLiteral: {
        enterOuterAlt(_localctx, 3);
        setState(1117);
        numberLiteral();
        break;
      }

      case CypherParser::StringLiteral: {
        enterOuterAlt(_localctx, 4);
        setState(1118);
        match(CypherParser::StringLiteral);
        break;
      }

      case CypherParser::LBRACE: {
        enterOuterAlt(_localctx, 5);
        setState(1119);
        mapLiteral();
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

//----------------- BooleanLiteralContext ------------------------------------------------------------------

CypherParser::BooleanLiteralContext::BooleanLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::BooleanLiteralContext::K_TRUE() {
  return getToken(CypherParser::K_TRUE, 0);
}

tree::TerminalNode* CypherParser::BooleanLiteralContext::K_FALSE() {
  return getToken(CypherParser::K_FALSE, 0);
}


size_t CypherParser::BooleanLiteralContext::getRuleIndex() const {
  return CypherParser::RuleBooleanLiteral;
}


std::any CypherParser::BooleanLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitBooleanLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::BooleanLiteralContext* CypherParser::booleanLiteral() {
  BooleanLiteralContext *_localctx = _tracker.createInstance<BooleanLiteralContext>(_ctx, getState());
  enterRule(_localctx, 174, CypherParser::RuleBooleanLiteral);
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
    setState(1122);
    _la = _input->LA(1);
    if (!(_la == CypherParser::K_FALSE

    || _la == CypherParser::K_TRUE)) {
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

//----------------- NumberLiteralContext ------------------------------------------------------------------

CypherParser::NumberLiteralContext::NumberLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::NumberLiteralContext::DoubleLiteral() {
  return getToken(CypherParser::DoubleLiteral, 0);
}

CypherParser::IntegerLiteralContext* CypherParser::NumberLiteralContext::integerLiteral() {
  return getRuleContext<CypherParser::IntegerLiteralContext>(0);
}


size_t CypherParser::NumberLiteralContext::getRuleIndex() const {
  return CypherParser::RuleNumberLiteral;
}


std::any CypherParser::NumberLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNumberLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NumberLiteralContext* CypherParser::numberLiteral() {
  NumberLiteralContext *_localctx = _tracker.createInstance<NumberLiteralContext>(_ctx, getState());
  enterRule(_localctx, 176, CypherParser::RuleNumberLiteral);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(1126);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::DoubleLiteral: {
        enterOuterAlt(_localctx, 1);
        setState(1124);
        match(CypherParser::DoubleLiteral);
        break;
      }

      case CypherParser::HexInteger:
      case CypherParser::OctalInteger:
      case CypherParser::DecimalInteger: {
        enterOuterAlt(_localctx, 2);
        setState(1125);
        integerLiteral();
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

//----------------- IntegerLiteralContext ------------------------------------------------------------------

CypherParser::IntegerLiteralContext::IntegerLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::IntegerLiteralContext::DecimalInteger() {
  return getToken(CypherParser::DecimalInteger, 0);
}

tree::TerminalNode* CypherParser::IntegerLiteralContext::HexInteger() {
  return getToken(CypherParser::HexInteger, 0);
}

tree::TerminalNode* CypherParser::IntegerLiteralContext::OctalInteger() {
  return getToken(CypherParser::OctalInteger, 0);
}


size_t CypherParser::IntegerLiteralContext::getRuleIndex() const {
  return CypherParser::RuleIntegerLiteral;
}


std::any CypherParser::IntegerLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitIntegerLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::IntegerLiteralContext* CypherParser::integerLiteral() {
  IntegerLiteralContext *_localctx = _tracker.createInstance<IntegerLiteralContext>(_ctx, getState());
  enterRule(_localctx, 178, CypherParser::RuleIntegerLiteral);
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
    setState(1128);
    _la = _input->LA(1);
    if (!(((((_la - 119) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 119)) & 7) != 0))) {
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

//----------------- MapLiteralContext ------------------------------------------------------------------

CypherParser::MapLiteralContext::MapLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::MapLiteralContext::LBRACE() {
  return getToken(CypherParser::LBRACE, 0);
}

tree::TerminalNode* CypherParser::MapLiteralContext::RBRACE() {
  return getToken(CypherParser::RBRACE, 0);
}

std::vector<CypherParser::PropertyKeyNameContext *> CypherParser::MapLiteralContext::propertyKeyName() {
  return getRuleContexts<CypherParser::PropertyKeyNameContext>();
}

CypherParser::PropertyKeyNameContext* CypherParser::MapLiteralContext::propertyKeyName(size_t i) {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::MapLiteralContext::COLON() {
  return getTokens(CypherParser::COLON);
}

tree::TerminalNode* CypherParser::MapLiteralContext::COLON(size_t i) {
  return getToken(CypherParser::COLON, i);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::MapLiteralContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::MapLiteralContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::MapLiteralContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::MapLiteralContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::MapLiteralContext::getRuleIndex() const {
  return CypherParser::RuleMapLiteral;
}


std::any CypherParser::MapLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitMapLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MapLiteralContext* CypherParser::mapLiteral() {
  MapLiteralContext *_localctx = _tracker.createInstance<MapLiteralContext>(_ctx, getState());
  enterRule(_localctx, 180, CypherParser::RuleMapLiteral);
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
    setState(1130);
    match(CypherParser::LBRACE);
    setState(1144);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 576460752840294399) != 0)) {
      setState(1131);
      propertyKeyName();
      setState(1132);
      match(CypherParser::COLON);
      setState(1133);
      expression();
      setState(1141);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(1134);
        match(CypherParser::COMMA);
        setState(1135);
        propertyKeyName();
        setState(1136);
        match(CypherParser::COLON);
        setState(1137);
        expression();
        setState(1143);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(1146);
    match(CypherParser::RBRACE);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ListLiteralContext ------------------------------------------------------------------

CypherParser::ListLiteralContext::ListLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ListLiteralContext::LBRACK() {
  return getToken(CypherParser::LBRACK, 0);
}

tree::TerminalNode* CypherParser::ListLiteralContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::ListLiteralContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::ListLiteralContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ListLiteralContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::ListLiteralContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::ListLiteralContext::getRuleIndex() const {
  return CypherParser::RuleListLiteral;
}


std::any CypherParser::ListLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitListLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ListLiteralContext* CypherParser::listLiteral() {
  ListLiteralContext *_localctx = _tracker.createInstance<ListLiteralContext>(_ctx, getState());
  enterRule(_localctx, 182, CypherParser::RuleListLiteral);
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
    setState(1148);
    match(CypherParser::LBRACK);
    setState(1157);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2274410377494134783) != 0)) {
      setState(1149);
      expression();
      setState(1154);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(1150);
        match(CypherParser::COMMA);
        setState(1151);
        expression();
        setState(1156);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(1159);
    match(CypherParser::RBRACK);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ParameterContext ------------------------------------------------------------------

CypherParser::ParameterContext::ParameterContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ParameterContext::DOLLAR() {
  return getToken(CypherParser::DOLLAR, 0);
}

CypherParser::SymbolicNameContext* CypherParser::ParameterContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

tree::TerminalNode* CypherParser::ParameterContext::DecimalInteger() {
  return getToken(CypherParser::DecimalInteger, 0);
}


size_t CypherParser::ParameterContext::getRuleIndex() const {
  return CypherParser::RuleParameter;
}


std::any CypherParser::ParameterContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitParameter(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ParameterContext* CypherParser::parameter() {
  ParameterContext *_localctx = _tracker.createInstance<ParameterContext>(_ctx, getState());
  enterRule(_localctx, 184, CypherParser::RuleParameter);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1161);
    match(CypherParser::DOLLAR);
    setState(1164);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
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
        setState(1162);
        symbolicName();
        break;
      }

      case CypherParser::DecimalInteger: {
        setState(1163);
        match(CypherParser::DecimalInteger);
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

//----------------- ListComprehensionContext ------------------------------------------------------------------

CypherParser::ListComprehensionContext::ListComprehensionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ListComprehensionContext::LBRACK() {
  return getToken(CypherParser::LBRACK, 0);
}

CypherParser::VariableContext* CypherParser::ListComprehensionContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::ListComprehensionContext::K_IN() {
  return getToken(CypherParser::K_IN, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::ListComprehensionContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::ListComprehensionContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

tree::TerminalNode* CypherParser::ListComprehensionContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}

tree::TerminalNode* CypherParser::ListComprehensionContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

tree::TerminalNode* CypherParser::ListComprehensionContext::PIPE() {
  return getToken(CypherParser::PIPE, 0);
}


size_t CypherParser::ListComprehensionContext::getRuleIndex() const {
  return CypherParser::RuleListComprehension;
}


std::any CypherParser::ListComprehensionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitListComprehension(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ListComprehensionContext* CypherParser::listComprehension() {
  ListComprehensionContext *_localctx = _tracker.createInstance<ListComprehensionContext>(_ctx, getState());
  enterRule(_localctx, 186, CypherParser::RuleListComprehension);
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
    setState(1166);
    match(CypherParser::LBRACK);
    setState(1167);
    variable();
    setState(1168);
    match(CypherParser::K_IN);
    setState(1169);
    expression();
    setState(1172);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(1170);
      match(CypherParser::K_WHERE);
      setState(1171);
      antlrcpp::downCast<ListComprehensionContext *>(_localctx)->whereExpression = expression();
    }
    setState(1176);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::PIPE) {
      setState(1174);
      match(CypherParser::PIPE);
      setState(1175);
      antlrcpp::downCast<ListComprehensionContext *>(_localctx)->mapExpression = expression();
    }
    setState(1178);
    match(CypherParser::RBRACK);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ReduceExpressionContext ------------------------------------------------------------------

CypherParser::ReduceExpressionContext::ReduceExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ReduceExpressionContext::K_REDUCE() {
  return getToken(CypherParser::K_REDUCE, 0);
}

tree::TerminalNode* CypherParser::ReduceExpressionContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

std::vector<CypherParser::VariableContext *> CypherParser::ReduceExpressionContext::variable() {
  return getRuleContexts<CypherParser::VariableContext>();
}

CypherParser::VariableContext* CypherParser::ReduceExpressionContext::variable(size_t i) {
  return getRuleContext<CypherParser::VariableContext>(i);
}

tree::TerminalNode* CypherParser::ReduceExpressionContext::EQ() {
  return getToken(CypherParser::EQ, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::ReduceExpressionContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::ReduceExpressionContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

tree::TerminalNode* CypherParser::ReduceExpressionContext::COMMA() {
  return getToken(CypherParser::COMMA, 0);
}

tree::TerminalNode* CypherParser::ReduceExpressionContext::K_IN() {
  return getToken(CypherParser::K_IN, 0);
}

tree::TerminalNode* CypherParser::ReduceExpressionContext::PIPE() {
  return getToken(CypherParser::PIPE, 0);
}

tree::TerminalNode* CypherParser::ReduceExpressionContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}


size_t CypherParser::ReduceExpressionContext::getRuleIndex() const {
  return CypherParser::RuleReduceExpression;
}


std::any CypherParser::ReduceExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitReduceExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ReduceExpressionContext* CypherParser::reduceExpression() {
  ReduceExpressionContext *_localctx = _tracker.createInstance<ReduceExpressionContext>(_ctx, getState());
  enterRule(_localctx, 188, CypherParser::RuleReduceExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1180);
    match(CypherParser::K_REDUCE);
    setState(1181);
    match(CypherParser::LPAREN);
    setState(1182);
    variable();
    setState(1183);
    match(CypherParser::EQ);
    setState(1184);
    expression();
    setState(1185);
    match(CypherParser::COMMA);
    setState(1186);
    variable();
    setState(1187);
    match(CypherParser::K_IN);
    setState(1188);
    expression();
    setState(1189);
    match(CypherParser::PIPE);
    setState(1190);
    expression();
    setState(1191);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ShortestPathExpressionContext ------------------------------------------------------------------

CypherParser::ShortestPathExpressionContext::ShortestPathExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ShortestPathExpressionContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PatternElementContext* CypherParser::ShortestPathExpressionContext::patternElement() {
  return getRuleContext<CypherParser::PatternElementContext>(0);
}

tree::TerminalNode* CypherParser::ShortestPathExpressionContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

tree::TerminalNode* CypherParser::ShortestPathExpressionContext::K_SHORTESTPATH() {
  return getToken(CypherParser::K_SHORTESTPATH, 0);
}

tree::TerminalNode* CypherParser::ShortestPathExpressionContext::K_ALLSHORTESTPATHS() {
  return getToken(CypherParser::K_ALLSHORTESTPATHS, 0);
}


size_t CypherParser::ShortestPathExpressionContext::getRuleIndex() const {
  return CypherParser::RuleShortestPathExpression;
}


std::any CypherParser::ShortestPathExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitShortestPathExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ShortestPathExpressionContext* CypherParser::shortestPathExpression() {
  ShortestPathExpressionContext *_localctx = _tracker.createInstance<ShortestPathExpressionContext>(_ctx, getState());
  enterRule(_localctx, 190, CypherParser::RuleShortestPathExpression);
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
    setState(1193);
    _la = _input->LA(1);
    if (!(_la == CypherParser::K_SHORTESTPATH

    || _la == CypherParser::K_ALLSHORTESTPATHS)) {
    _errHandler->recoverInline(this);
    }
    else {
      _errHandler->reportMatch(this);
      consume();
    }
    setState(1194);
    match(CypherParser::LPAREN);
    setState(1195);
    patternElement();
    setState(1196);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- MapProjectionContext ------------------------------------------------------------------

CypherParser::MapProjectionContext::MapProjectionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::VariableContext* CypherParser::MapProjectionContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::MapProjectionContext::LBRACE() {
  return getToken(CypherParser::LBRACE, 0);
}

std::vector<CypherParser::MapProjectionElementContext *> CypherParser::MapProjectionContext::mapProjectionElement() {
  return getRuleContexts<CypherParser::MapProjectionElementContext>();
}

CypherParser::MapProjectionElementContext* CypherParser::MapProjectionContext::mapProjectionElement(size_t i) {
  return getRuleContext<CypherParser::MapProjectionElementContext>(i);
}

tree::TerminalNode* CypherParser::MapProjectionContext::RBRACE() {
  return getToken(CypherParser::RBRACE, 0);
}

std::vector<tree::TerminalNode *> CypherParser::MapProjectionContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::MapProjectionContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::MapProjectionContext::getRuleIndex() const {
  return CypherParser::RuleMapProjection;
}


std::any CypherParser::MapProjectionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitMapProjection(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MapProjectionContext* CypherParser::mapProjection() {
  MapProjectionContext *_localctx = _tracker.createInstance<MapProjectionContext>(_ctx, getState());
  enterRule(_localctx, 192, CypherParser::RuleMapProjection);
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
    setState(1198);
    variable();
    setState(1199);
    match(CypherParser::LBRACE);
    setState(1200);
    mapProjectionElement();
    setState(1205);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(1201);
      match(CypherParser::COMMA);
      setState(1202);
      mapProjectionElement();
      setState(1207);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
    setState(1208);
    match(CypherParser::RBRACE);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- MapProjectionElementContext ------------------------------------------------------------------

CypherParser::MapProjectionElementContext::MapProjectionElementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::MapProjectionElementContext::DOT() {
  return getToken(CypherParser::DOT, 0);
}

tree::TerminalNode* CypherParser::MapProjectionElementContext::MULTIPLY() {
  return getToken(CypherParser::MULTIPLY, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::MapProjectionElementContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::MapProjectionElementContext::COLON() {
  return getToken(CypherParser::COLON, 0);
}

CypherParser::ExpressionContext* CypherParser::MapProjectionElementContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}


size_t CypherParser::MapProjectionElementContext::getRuleIndex() const {
  return CypherParser::RuleMapProjectionElement;
}


std::any CypherParser::MapProjectionElementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitMapProjectionElement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MapProjectionElementContext* CypherParser::mapProjectionElement() {
  MapProjectionElementContext *_localctx = _tracker.createInstance<MapProjectionElementContext>(_ctx, getState());
  enterRule(_localctx, 194, CypherParser::RuleMapProjectionElement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(1218);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 140, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(1210);
      match(CypherParser::DOT);
      setState(1211);
      match(CypherParser::MULTIPLY);
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(1212);
      propertyKeyName();
      setState(1213);
      match(CypherParser::COLON);
      setState(1214);
      expression();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(1216);
      match(CypherParser::DOT);
      setState(1217);
      propertyKeyName();
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

//----------------- PatternComprehensionContext ------------------------------------------------------------------

CypherParser::PatternComprehensionContext::PatternComprehensionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::PatternComprehensionContext::LBRACK() {
  return getToken(CypherParser::LBRACK, 0);
}

CypherParser::PatternElementContext* CypherParser::PatternComprehensionContext::patternElement() {
  return getRuleContext<CypherParser::PatternElementContext>(0);
}

tree::TerminalNode* CypherParser::PatternComprehensionContext::PIPE() {
  return getToken(CypherParser::PIPE, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::PatternComprehensionContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::PatternComprehensionContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

tree::TerminalNode* CypherParser::PatternComprehensionContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}

tree::TerminalNode* CypherParser::PatternComprehensionContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}


size_t CypherParser::PatternComprehensionContext::getRuleIndex() const {
  return CypherParser::RulePatternComprehension;
}


std::any CypherParser::PatternComprehensionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPatternComprehension(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternComprehensionContext* CypherParser::patternComprehension() {
  PatternComprehensionContext *_localctx = _tracker.createInstance<PatternComprehensionContext>(_ctx, getState());
  enterRule(_localctx, 196, CypherParser::RulePatternComprehension);
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
    setState(1220);
    match(CypherParser::LBRACK);
    setState(1221);
    patternElement();
    setState(1224);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(1222);
      match(CypherParser::K_WHERE);
      setState(1223);
      expression();
    }
    setState(1226);
    match(CypherParser::PIPE);
    setState(1227);
    expression();
    setState(1228);
    match(CypherParser::RBRACK);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

