//----------------- VariableContext ------------------------------------------------------------------

CypherParser::VariableContext::VariableContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SymbolicNameContext* CypherParser::VariableContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}


size_t CypherParser::VariableContext::getRuleIndex() const {
  return CypherParser::RuleVariable;
}


std::any CypherParser::VariableContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitVariable(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::VariableContext* CypherParser::variable() {
  VariableContext *_localctx = _tracker.createInstance<VariableContext>(_ctx, getState());
  enterRule(_localctx, 152, CypherParser::RuleVariable);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1057);
    symbolicName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- LabelNameContext ------------------------------------------------------------------

CypherParser::LabelNameContext::LabelNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SchemaNameContext* CypherParser::LabelNameContext::schemaName() {
  return getRuleContext<CypherParser::SchemaNameContext>(0);
}


size_t CypherParser::LabelNameContext::getRuleIndex() const {
  return CypherParser::RuleLabelName;
}


std::any CypherParser::LabelNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitLabelName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::LabelNameContext* CypherParser::labelName() {
  LabelNameContext *_localctx = _tracker.createInstance<LabelNameContext>(_ctx, getState());
  enterRule(_localctx, 154, CypherParser::RuleLabelName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1059);
    schemaName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- RelTypeNameContext ------------------------------------------------------------------

CypherParser::RelTypeNameContext::RelTypeNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SchemaNameContext* CypherParser::RelTypeNameContext::schemaName() {
  return getRuleContext<CypherParser::SchemaNameContext>(0);
}


size_t CypherParser::RelTypeNameContext::getRuleIndex() const {
  return CypherParser::RuleRelTypeName;
}


std::any CypherParser::RelTypeNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRelTypeName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RelTypeNameContext* CypherParser::relTypeName() {
  RelTypeNameContext *_localctx = _tracker.createInstance<RelTypeNameContext>(_ctx, getState());
  enterRule(_localctx, 156, CypherParser::RuleRelTypeName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1061);
    schemaName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PropertyKeyNameContext ------------------------------------------------------------------

CypherParser::PropertyKeyNameContext::PropertyKeyNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SchemaNameContext* CypherParser::PropertyKeyNameContext::schemaName() {
  return getRuleContext<CypherParser::SchemaNameContext>(0);
}


size_t CypherParser::PropertyKeyNameContext::getRuleIndex() const {
  return CypherParser::RulePropertyKeyName;
}


std::any CypherParser::PropertyKeyNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPropertyKeyName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PropertyKeyNameContext* CypherParser::propertyKeyName() {
  PropertyKeyNameContext *_localctx = _tracker.createInstance<PropertyKeyNameContext>(_ctx, getState());
  enterRule(_localctx, 158, CypherParser::RulePropertyKeyName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1063);
    schemaName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ProcedureNameContext ------------------------------------------------------------------

CypherParser::ProcedureNameContext::ProcedureNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::NamespaceContext* CypherParser::ProcedureNameContext::namespace_() {
  return getRuleContext<CypherParser::NamespaceContext>(0);
}

CypherParser::SymbolicNameContext* CypherParser::ProcedureNameContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}


size_t CypherParser::ProcedureNameContext::getRuleIndex() const {
  return CypherParser::RuleProcedureName;
}


std::any CypherParser::ProcedureNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProcedureName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProcedureNameContext* CypherParser::procedureName() {
  ProcedureNameContext *_localctx = _tracker.createInstance<ProcedureNameContext>(_ctx, getState());
  enterRule(_localctx, 160, CypherParser::RuleProcedureName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1065);
    namespace_();
    setState(1066);
    symbolicName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ProcedureResultFieldContext ------------------------------------------------------------------

CypherParser::ProcedureResultFieldContext::ProcedureResultFieldContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SymbolicNameContext* CypherParser::ProcedureResultFieldContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}


size_t CypherParser::ProcedureResultFieldContext::getRuleIndex() const {
  return CypherParser::RuleProcedureResultField;
}


std::any CypherParser::ProcedureResultFieldContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProcedureResultField(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProcedureResultFieldContext* CypherParser::procedureResultField() {
  ProcedureResultFieldContext *_localctx = _tracker.createInstance<ProcedureResultFieldContext>(_ctx, getState());
  enterRule(_localctx, 162, CypherParser::RuleProcedureResultField);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1068);
    symbolicName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- FunctionNameContext ------------------------------------------------------------------

CypherParser::FunctionNameContext::FunctionNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::NamespaceContext* CypherParser::FunctionNameContext::namespace_() {
  return getRuleContext<CypherParser::NamespaceContext>(0);
}

CypherParser::SymbolicNameContext* CypherParser::FunctionNameContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}


size_t CypherParser::FunctionNameContext::getRuleIndex() const {
  return CypherParser::RuleFunctionName;
}


std::any CypherParser::FunctionNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitFunctionName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::FunctionNameContext* CypherParser::functionName() {
  FunctionNameContext *_localctx = _tracker.createInstance<FunctionNameContext>(_ctx, getState());
  enterRule(_localctx, 164, CypherParser::RuleFunctionName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(1070);
    namespace_();
    setState(1071);
    symbolicName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- NamespaceContext ------------------------------------------------------------------

CypherParser::NamespaceContext::NamespaceContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::SymbolicNameContext *> CypherParser::NamespaceContext::symbolicName() {
  return getRuleContexts<CypherParser::SymbolicNameContext>();
}

CypherParser::SymbolicNameContext* CypherParser::NamespaceContext::symbolicName(size_t i) {
  return getRuleContext<CypherParser::SymbolicNameContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::NamespaceContext::DOT() {
  return getTokens(CypherParser::DOT);
}

tree::TerminalNode* CypherParser::NamespaceContext::DOT(size_t i) {
  return getToken(CypherParser::DOT, i);
}


size_t CypherParser::NamespaceContext::getRuleIndex() const {
  return CypherParser::RuleNamespace;
}


std::any CypherParser::NamespaceContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNamespace(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NamespaceContext* CypherParser::namespace_() {
  NamespaceContext *_localctx = _tracker.createInstance<NamespaceContext>(_ctx, getState());
  enterRule(_localctx, 166, CypherParser::RuleNamespace);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    size_t alt;
    enterOuterAlt(_localctx, 1);
    setState(1078);
    _errHandler->sync(this);
    alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 128, _ctx);
    while (alt != 2 && alt != atn::ATN::INVALID_ALT_NUMBER) {
      if (alt == 1) {
        setState(1073);
        symbolicName();
        setState(1074);
        match(CypherParser::DOT); 
      }
      setState(1080);
      _errHandler->sync(this);
      alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 128, _ctx);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- SchemaNameContext ------------------------------------------------------------------

CypherParser::SchemaNameContext::SchemaNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SymbolicNameContext* CypherParser::SchemaNameContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_MATCH() {
  return getToken(CypherParser::K_MATCH, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_RETURN() {
  return getToken(CypherParser::K_RETURN, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_BEGIN() {
  return getToken(CypherParser::K_BEGIN, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_COMMIT() {
  return getToken(CypherParser::K_COMMIT, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_ROLLBACK() {
  return getToken(CypherParser::K_ROLLBACK, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_TRANSACTION() {
  return getToken(CypherParser::K_TRANSACTION, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_KEY() {
  return getToken(CypherParser::K_KEY, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_NODE() {
  return getToken(CypherParser::K_NODE, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_CONSTRAINT() {
  return getToken(CypherParser::K_CONSTRAINT, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_BOOLEAN() {
  return getToken(CypherParser::K_BOOLEAN, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_INTEGER() {
  return getToken(CypherParser::K_INTEGER, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_FLOAT() {
  return getToken(CypherParser::K_FLOAT, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_STRING() {
  return getToken(CypherParser::K_STRING, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_LIST() {
  return getToken(CypherParser::K_LIST, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_MAP() {
  return getToken(CypherParser::K_MAP, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_FOREACH() {
  return getToken(CypherParser::K_FOREACH, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_LOAD() {
  return getToken(CypherParser::K_LOAD, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_CSV() {
  return getToken(CypherParser::K_CSV, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_HEADERS() {
  return getToken(CypherParser::K_HEADERS, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_FROM() {
  return getToken(CypherParser::K_FROM, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_FIELDTERMINATOR() {
  return getToken(CypherParser::K_FIELDTERMINATOR, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_TRANSACTIONS() {
  return getToken(CypherParser::K_TRANSACTIONS, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_ROWS() {
  return getToken(CypherParser::K_ROWS, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_SHORTESTPATH() {
  return getToken(CypherParser::K_SHORTESTPATH, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_ALLSHORTESTPATHS() {
  return getToken(CypherParser::K_ALLSHORTESTPATHS, 0);
}


size_t CypherParser::SchemaNameContext::getRuleIndex() const {
  return CypherParser::RuleSchemaName;
}


std::any CypherParser::SchemaNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSchemaName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SchemaNameContext* CypherParser::schemaName() {
  SchemaNameContext *_localctx = _tracker.createInstance<SchemaNameContext>(_ctx, getState());
  enterRule(_localctx, 168, CypherParser::RuleSchemaName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(1111);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 129, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(1081);
      symbolicName();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(1082);
      match(CypherParser::K_INDEX);
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(1083);
      match(CypherParser::K_ON);
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(1084);
      match(CypherParser::K_MATCH);
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(1085);
      match(CypherParser::K_CREATE);
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(1086);
      match(CypherParser::K_WHERE);
      break;
    }

    case 7: {
      enterOuterAlt(_localctx, 7);
      setState(1087);
      match(CypherParser::K_RETURN);
      break;
    }

    case 8: {
      enterOuterAlt(_localctx, 8);
      setState(1088);
      match(CypherParser::K_BEGIN);
      break;
    }

    case 9: {
      enterOuterAlt(_localctx, 9);
      setState(1089);
      match(CypherParser::K_COMMIT);
      break;
    }

    case 10: {
      enterOuterAlt(_localctx, 10);
      setState(1090);
      match(CypherParser::K_ROLLBACK);
      break;
    }

    case 11: {
      enterOuterAlt(_localctx, 11);
      setState(1091);
      match(CypherParser::K_TRANSACTION);
      break;
    }

    case 12: {
      enterOuterAlt(_localctx, 12);
      setState(1092);
      match(CypherParser::K_KEY);
      break;
    }

    case 13: {
      enterOuterAlt(_localctx, 13);
      setState(1093);
      match(CypherParser::K_NODE);
      break;
    }

    case 14: {
      enterOuterAlt(_localctx, 14);
      setState(1094);
      match(CypherParser::K_CONSTRAINT);
      break;
    }

    case 15: {
      enterOuterAlt(_localctx, 15);
      setState(1095);
      match(CypherParser::K_BOOLEAN);
      break;
    }

    case 16: {
      enterOuterAlt(_localctx, 16);
      setState(1096);
      match(CypherParser::K_INTEGER);
      break;
    }

    case 17: {
      enterOuterAlt(_localctx, 17);
      setState(1097);
      match(CypherParser::K_FLOAT);
      break;
    }

    case 18: {
      enterOuterAlt(_localctx, 18);
      setState(1098);
      match(CypherParser::K_STRING);
      break;
    }

    case 19: {
      enterOuterAlt(_localctx, 19);
      setState(1099);
      match(CypherParser::K_LIST);
      break;
    }

    case 20: {
      enterOuterAlt(_localctx, 20);
      setState(1100);
      match(CypherParser::K_MAP);
      break;
    }

    case 21: {
      enterOuterAlt(_localctx, 21);
      setState(1101);
      match(CypherParser::K_FOREACH);
      break;
    }

    case 22: {
      enterOuterAlt(_localctx, 22);
      setState(1102);
      match(CypherParser::K_LOAD);
      break;
    }

    case 23: {
      enterOuterAlt(_localctx, 23);
      setState(1103);
      match(CypherParser::K_CSV);
      break;
    }

    case 24: {
      enterOuterAlt(_localctx, 24);
      setState(1104);
      match(CypherParser::K_HEADERS);
      break;
    }

    case 25: {
      enterOuterAlt(_localctx, 25);
      setState(1105);
      match(CypherParser::K_FROM);
      break;
    }

    case 26: {
      enterOuterAlt(_localctx, 26);
      setState(1106);
      match(CypherParser::K_FIELDTERMINATOR);
      break;
    }

    case 27: {
      enterOuterAlt(_localctx, 27);
      setState(1107);
      match(CypherParser::K_TRANSACTIONS);
      break;
    }

    case 28: {
      enterOuterAlt(_localctx, 28);
      setState(1108);
      match(CypherParser::K_ROWS);
      break;
    }

    case 29: {
      enterOuterAlt(_localctx, 29);
      setState(1109);
      match(CypherParser::K_SHORTESTPATH);
      break;
    }

    case 30: {
      enterOuterAlt(_localctx, 30);
      setState(1110);
      match(CypherParser::K_ALLSHORTESTPATHS);
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

//----------------- SymbolicNameContext ------------------------------------------------------------------

CypherParser::SymbolicNameContext::SymbolicNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::SymbolicNameContext::ID() {
  return getToken(CypherParser::ID, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CALL() {
  return getToken(CypherParser::K_CALL, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_YIELD() {
  return getToken(CypherParser::K_YIELD, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DELETE() {
  return getToken(CypherParser::K_DELETE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DETACH() {
  return getToken(CypherParser::K_DETACH, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_EXISTS() {
  return getToken(CypherParser::K_EXISTS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_LIMIT() {
  return getToken(CypherParser::K_LIMIT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_MATCH() {
  return getToken(CypherParser::K_MATCH, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_MERGE() {
  return getToken(CypherParser::K_MERGE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_OPTIONAL() {
  return getToken(CypherParser::K_OPTIONAL, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ORDER() {
  return getToken(CypherParser::K_ORDER, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_BY() {
  return getToken(CypherParser::K_BY, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SKIP() {
  return getToken(CypherParser::K_SKIP, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ASCENDING() {
  return getToken(CypherParser::K_ASCENDING, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ASC() {
  return getToken(CypherParser::K_ASC, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DESCENDING() {
  return getToken(CypherParser::K_DESCENDING, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DESC() {
  return getToken(CypherParser::K_DESC, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_REMOVE() {
  return getToken(CypherParser::K_REMOVE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_RETURN() {
  return getToken(CypherParser::K_RETURN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SET() {
  return getToken(CypherParser::K_SET, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_WITH() {
  return getToken(CypherParser::K_WITH, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_UNION() {
  return getToken(CypherParser::K_UNION, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_UNWIND() {
  return getToken(CypherParser::K_UNWIND, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_AND() {
  return getToken(CypherParser::K_AND, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CONTAINS() {
  return getToken(CypherParser::K_CONTAINS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DISTINCT() {
  return getToken(CypherParser::K_DISTINCT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ENDS() {
  return getToken(CypherParser::K_ENDS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_IN() {
  return getToken(CypherParser::K_IN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_IS() {
  return getToken(CypherParser::K_IS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_NOT() {
  return getToken(CypherParser::K_NOT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_OR() {
  return getToken(CypherParser::K_OR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_STARTS() {
  return getToken(CypherParser::K_STARTS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_XOR() {
  return getToken(CypherParser::K_XOR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_BETWEEN() {
  return getToken(CypherParser::K_BETWEEN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_FALSE() {
  return getToken(CypherParser::K_FALSE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_TRUE() {
  return getToken(CypherParser::K_TRUE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_NULL() {
  return getToken(CypherParser::K_NULL, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CASE() {
  return getToken(CypherParser::K_CASE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_WHEN() {
  return getToken(CypherParser::K_WHEN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_THEN() {
  return getToken(CypherParser::K_THEN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ELSE() {
  return getToken(CypherParser::K_ELSE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_END() {
  return getToken(CypherParser::K_END, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_COUNT() {
  return getToken(CypherParser::K_COUNT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_FILTER() {
  return getToken(CypherParser::K_FILTER, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_EXTRACT() {
  return getToken(CypherParser::K_EXTRACT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ANY() {
  return getToken(CypherParser::K_ANY, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_NONE() {
  return getToken(CypherParser::K_NONE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SINGLE() {
  return getToken(CypherParser::K_SINGLE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ALL() {
  return getToken(CypherParser::K_ALL, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_REDUCE() {
  return getToken(CypherParser::K_REDUCE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SHOW() {
  return getToken(CypherParser::K_SHOW, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_FOR() {
  return getToken(CypherParser::K_FOR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CONSTRAINT() {
  return getToken(CypherParser::K_CONSTRAINT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DO() {
  return getToken(CypherParser::K_DO, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_REQUIRE() {
  return getToken(CypherParser::K_REQUIRE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_UNIQUE() {
  return getToken(CypherParser::K_UNIQUE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_MANDATORY() {
  return getToken(CypherParser::K_MANDATORY, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SCALAR() {
  return getToken(CypherParser::K_SCALAR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_OF() {
  return getToken(CypherParser::K_OF, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ADD() {
  return getToken(CypherParser::K_ADD, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_KEY() {
  return getToken(CypherParser::K_KEY, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_NODE() {
  return getToken(CypherParser::K_NODE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_BOOLEAN() {
  return getToken(CypherParser::K_BOOLEAN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_INTEGER() {
  return getToken(CypherParser::K_INTEGER, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_FLOAT() {
  return getToken(CypherParser::K_FLOAT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_STRING() {
  return getToken(CypherParser::K_STRING, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_LIST() {
  return getToken(CypherParser::K_LIST, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_MAP() {
  return getToken(CypherParser::K_MAP, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_IF() {
  return getToken(CypherParser::K_IF, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_VECTOR() {
  return getToken(CypherParser::K_VECTOR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_OPTIONS() {
  return getToken(CypherParser::K_OPTIONS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_BEGIN() {
  return getToken(CypherParser::K_BEGIN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_COMMIT() {
  return getToken(CypherParser::K_COMMIT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ROLLBACK() {
  return getToken(CypherParser::K_ROLLBACK, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_TRANSACTION() {
  return getToken(CypherParser::K_TRANSACTION, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_EXPLAIN() {
  return getToken(CypherParser::K_EXPLAIN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_PROFILE() {
  return getToken(CypherParser::K_PROFILE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_FOREACH() {
  return getToken(CypherParser::K_FOREACH, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_LOAD() {
  return getToken(CypherParser::K_LOAD, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CSV() {
  return getToken(CypherParser::K_CSV, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_HEADERS() {
  return getToken(CypherParser::K_HEADERS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_FROM() {
  return getToken(CypherParser::K_FROM, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_FIELDTERMINATOR() {
  return getToken(CypherParser::K_FIELDTERMINATOR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_TRANSACTIONS() {
  return getToken(CypherParser::K_TRANSACTIONS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ROWS() {
  return getToken(CypherParser::K_ROWS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SHORTESTPATH() {
  return getToken(CypherParser::K_SHORTESTPATH, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ALLSHORTESTPATHS() {
  return getToken(CypherParser::K_ALLSHORTESTPATHS, 0);
}


size_t CypherParser::SymbolicNameContext::getRuleIndex() const {
  return CypherParser::RuleSymbolicName;
}


std::any CypherParser::SymbolicNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSymbolicName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SymbolicNameContext* CypherParser::symbolicName() {
  SymbolicNameContext *_localctx = _tracker.createInstance<SymbolicNameContext>(_ctx, getState());
  enterRule(_localctx, 170, CypherParser::RuleSymbolicName);
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
    setState(1113);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 576460752840294399) != 0))) {
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

