//===--- ClangdLSPServer.cpp - LSP server ------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ClangdLSPServer.h"
#include "Diagnostics.h"
#include "SourceCode.h"
#include "Trace.h"
#include "URI.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ScopedPrinter.h"

namespace clang {
namespace clangd {
namespace {
class IgnoreCompletionError : public llvm::ErrorInfo<CancelledError> {
public:
  void log(llvm::raw_ostream &OS) const override {
    OS << "ignored auto-triggered completion, preceding char did not match";
  }
  std::error_code convertToErrorCode() const override {
    return std::make_error_code(std::errc::operation_canceled);
  }
};

void adjustSymbolKinds(llvm::MutableArrayRef<DocumentSymbol> Syms,
                       SymbolKindBitset Kinds) {
  for (auto &S : Syms) {
    S.kind = adjustKindToCapability(S.kind, Kinds);
    adjustSymbolKinds(S.children, Kinds);
  }
}

SymbolKindBitset defaultSymbolKinds() {
  SymbolKindBitset Defaults;
  for (size_t I = SymbolKindMin; I <= static_cast<size_t>(SymbolKind::Array);
       ++I)
    Defaults.set(I);
  return Defaults;
}

CompletionItemKindBitset defaultCompletionItemKinds() {
  CompletionItemKindBitset Defaults;
  for (size_t I = CompletionItemKindMin;
       I <= static_cast<size_t>(CompletionItemKind::Reference); ++I)
    Defaults.set(I);
  return Defaults;
}

} // namespace

// MessageHandler dispatches incoming LSP messages.
// It handles cross-cutting concerns:
//  - serializes/deserializes protocol objects to JSON
//  - logging of inbound messages
//  - cancellation handling
//  - basic call tracing
// MessageHandler ensures that initialize() is called before any other handler.
class ClangdLSPServer::MessageHandler : public Transport::MessageHandler {
public:
  MessageHandler(ClangdLSPServer &Server) : Server(Server) {}

  bool onNotify(llvm::StringRef Method, llvm::json::Value Params) override {
    log("<-- {0}", Method);
    if (Method == "exit")
      return false;
    if (!Server.Server)
      elog("Notification {0} before initialization", Method);
    else if (Method == "$/cancelRequest")
      onCancel(std::move(Params));
    else if (auto Handler = Notifications.lookup(Method))
      Handler(std::move(Params));
    else
      log("unhandled notification {0}", Method);
    return true;
  }

  bool onCall(llvm::StringRef Method, llvm::json::Value Params,
              llvm::json::Value ID) override {
    // Calls can be canceled by the client. Add cancellation context.
    WithContext WithCancel(cancelableRequestContext(ID));
    trace::Span Tracer(Method);
    SPAN_ATTACH(Tracer, "Params", Params);
    ReplyOnce Reply(ID, Method, &Server, Tracer.Args);
    log("<-- {0}({1})", Method, ID);
    if (!Server.Server && Method != "initialize") {
      elog("Call {0} before initialization.", Method);
      Reply(llvm::make_error<LSPError>("server not initialized",
                                       ErrorCode::ServerNotInitialized));
    } else if (auto Handler = Calls.lookup(Method))
      Handler(std::move(Params), std::move(Reply));
    else
      Reply(llvm::make_error<LSPError>("method not found",
                                       ErrorCode::MethodNotFound));
    return true;
  }

  bool onReply(llvm::json::Value ID,
               llvm::Expected<llvm::json::Value> Result) override {
    // We ignore replies, just log them.
    if (Result)
      log("<-- reply({0})", ID);
    else
      log("<-- reply({0}) error: {1}", ID, llvm::toString(Result.takeError()));
    return true;
  }

  // Bind an LSP method name to a call.
  template <typename Param, typename Result>
  void bind(const char *Method,
            void (ClangdLSPServer::*Handler)(const Param &, Callback<Result>)) {
    Calls[Method] = [Method, Handler, this](llvm::json::Value RawParams,
                                            ReplyOnce Reply) {
      Param P;
      if (fromJSON(RawParams, P)) {
        (Server.*Handler)(P, std::move(Reply));
      } else {
        elog("Failed to decode {0} request.", Method);
        Reply(llvm::make_error<LSPError>("failed to decode request",
                                         ErrorCode::InvalidRequest));
      }
    };
  }

  // Bind an LSP method name to a notification.
  template <typename Param>
  void bind(const char *Method,
            void (ClangdLSPServer::*Handler)(const Param &)) {
    Notifications[Method] = [Method, Handler,
                             this](llvm::json::Value RawParams) {
      Param P;
      if (!fromJSON(RawParams, P)) {
        elog("Failed to decode {0} request.", Method);
        return;
      }
      trace::Span Tracer(Method);
      SPAN_ATTACH(Tracer, "Params", RawParams);
      (Server.*Handler)(P);
    };
  }

private:
  // Function object to reply to an LSP call.
  // Each instance must be called exactly once, otherwise:
  //  - the bug is logged, and (in debug mode) an assert will fire
  //  - if there was no reply, an error reply is sent
  //  - if there were multiple replies, only the first is sent
  class ReplyOnce {
    std::atomic<bool> Replied = {false};
    std::chrono::steady_clock::time_point Start;
    llvm::json::Value ID;
    std::string Method;
    ClangdLSPServer *Server; // Null when moved-from.
    llvm::json::Object *TraceArgs;

  public:
    ReplyOnce(const llvm::json::Value &ID, llvm::StringRef Method,
              ClangdLSPServer *Server, llvm::json::Object *TraceArgs)
        : Start(std::chrono::steady_clock::now()), ID(ID), Method(Method),
          Server(Server), TraceArgs(TraceArgs) {
      assert(Server);
    }
    ReplyOnce(ReplyOnce &&Other)
        : Replied(Other.Replied.load()), Start(Other.Start),
          ID(std::move(Other.ID)), Method(std::move(Other.Method)),
          Server(Other.Server), TraceArgs(Other.TraceArgs) {
      Other.Server = nullptr;
    }
    ReplyOnce &operator=(ReplyOnce &&) = delete;
    ReplyOnce(const ReplyOnce &) = delete;
    ReplyOnce &operator=(const ReplyOnce &) = delete;

    ~ReplyOnce() {
      if (Server && !Replied) {
        elog("No reply to message {0}({1})", Method, ID);
        assert(false && "must reply to all calls!");
        (*this)(llvm::make_error<LSPError>("server failed to reply",
                                           ErrorCode::InternalError));
      }
    }

    void operator()(llvm::Expected<llvm::json::Value> Reply) {
      assert(Server && "moved-from!");
      if (Replied.exchange(true)) {
        elog("Replied twice to message {0}({1})", Method, ID);
        assert(false && "must reply to each call only once!");
        return;
      }
      auto Duration = std::chrono::steady_clock::now() - Start;
      if (Reply) {
        log("--> reply:{0}({1}) {2:ms}", Method, ID, Duration);
        if (TraceArgs)
          (*TraceArgs)["Reply"] = *Reply;
        std::lock_guard<std::mutex> Lock(Server->TranspWriter);
        Server->Transp.reply(std::move(ID), std::move(Reply));
      } else {
        llvm::Error Err = Reply.takeError();
        log("--> reply:{0}({1}) {2:ms}, error: {3}", Method, ID, Duration, Err);
        if (TraceArgs)
          (*TraceArgs)["Error"] = llvm::to_string(Err);
        std::lock_guard<std::mutex> Lock(Server->TranspWriter);
        Server->Transp.reply(std::move(ID), std::move(Err));
      }
    }
  };

  llvm::StringMap<std::function<void(llvm::json::Value)>> Notifications;
  llvm::StringMap<std::function<void(llvm::json::Value, ReplyOnce)>> Calls;

  // Method calls may be cancelled by ID, so keep track of their state.
  // This needs a mutex: handlers may finish on a different thread, and that's
  // when we clean up entries in the map.
  mutable std::mutex RequestCancelersMutex;
  llvm::StringMap<std::pair<Canceler, /*Cookie*/ unsigned>> RequestCancelers;
  unsigned NextRequestCookie = 0; // To disambiguate reused IDs, see below.
  void onCancel(const llvm::json::Value &Params) {
    const llvm::json::Value *ID = nullptr;
    if (auto *O = Params.getAsObject())
      ID = O->get("id");
    if (!ID) {
      elog("Bad cancellation request: {0}", Params);
      return;
    }
    auto StrID = llvm::to_string(*ID);
    std::lock_guard<std::mutex> Lock(RequestCancelersMutex);
    auto It = RequestCancelers.find(StrID);
    if (It != RequestCancelers.end())
      It->second.first(); // Invoke the canceler.
  }
  // We run cancelable requests in a context that does two things:
  //  - allows cancellation using RequestCancelers[ID]
  //  - cleans up the entry in RequestCancelers when it's no longer needed
  // If a client reuses an ID, the last wins and the first cannot be canceled.
  Context cancelableRequestContext(const llvm::json::Value &ID) {
    auto Task = cancelableTask();
    auto StrID = llvm::to_string(ID);  // JSON-serialize ID for map key.
    auto Cookie = NextRequestCookie++; // No lock, only called on main thread.
    {
      std::lock_guard<std::mutex> Lock(RequestCancelersMutex);
      RequestCancelers[StrID] = {std::move(Task.second), Cookie};
    }
    // When the request ends, we can clean up the entry we just added.
    // The cookie lets us check that it hasn't been overwritten due to ID
    // reuse.
    return Task.first.derive(llvm::make_scope_exit([this, StrID, Cookie] {
      std::lock_guard<std::mutex> Lock(RequestCancelersMutex);
      auto It = RequestCancelers.find(StrID);
      if (It != RequestCancelers.end() && It->second.second == Cookie)
        RequestCancelers.erase(It);
    }));
  }

  ClangdLSPServer &Server;
};

// call(), notify(), and reply() wrap the Transport, adding logging and locking.
void ClangdLSPServer::call(llvm::StringRef Method, llvm::json::Value Params) {
  auto ID = NextCallID++;
  log("--> {0}({1})", Method, ID);
  // We currently don't handle responses, so no need to store ID anywhere.
  std::lock_guard<std::mutex> Lock(TranspWriter);
  Transp.call(Method, std::move(Params), ID);
}

void ClangdLSPServer::notify(llvm::StringRef Method, llvm::json::Value Params) {
  log("--> {0}", Method);
  std::lock_guard<std::mutex> Lock(TranspWriter);
  Transp.notify(Method, std::move(Params));
}

void ClangdLSPServer::onInitialize(const InitializeParams &Params,
                                   Callback<llvm::json::Value> Reply) {
  if (Params.rootUri && *Params.rootUri)
    ClangdServerOpts.WorkspaceRoot = Params.rootUri->file();
  else if (Params.rootPath && !Params.rootPath->empty())
    ClangdServerOpts.WorkspaceRoot = *Params.rootPath;
  if (Server)
    return Reply(llvm::make_error<LSPError>("server already initialized",
                                            ErrorCode::InvalidRequest));
  if (const auto &Dir = Params.initializationOptions.compilationDatabasePath)
    CompileCommandsDir = Dir;
  if (UseDirBasedCDB)
    BaseCDB = llvm::make_unique<DirectoryBasedGlobalCompilationDatabase>(
        CompileCommandsDir);
  CDB.emplace(BaseCDB.get(), Params.initializationOptions.fallbackFlags,
              ClangdServerOpts.ResourceDir);
  Server.emplace(*CDB, FSProvider, static_cast<DiagnosticsConsumer &>(*this),
                 ClangdServerOpts);
  applyConfiguration(Params.initializationOptions.ConfigSettings);

  CCOpts.EnableSnippets = Params.capabilities.CompletionSnippets;
  DiagOpts.EmbedFixesInDiagnostics = Params.capabilities.DiagnosticFixes;
  DiagOpts.SendDiagnosticCategory = Params.capabilities.DiagnosticCategory;
  if (Params.capabilities.WorkspaceSymbolKinds)
    SupportedSymbolKinds |= *Params.capabilities.WorkspaceSymbolKinds;
  if (Params.capabilities.CompletionItemKinds)
    SupportedCompletionItemKinds |= *Params.capabilities.CompletionItemKinds;
  SupportsCodeAction = Params.capabilities.CodeActionStructure;
  SupportsHierarchicalDocumentSymbol =
      Params.capabilities.HierarchicalDocumentSymbol;
  SupportFileStatus = Params.initializationOptions.FileStatus;
  Reply(llvm::json::Object{
      {{"capabilities",
        llvm::json::Object{
            {"textDocumentSync", (int)TextDocumentSyncKind::Incremental},
            {"documentFormattingProvider", true},
            {"documentRangeFormattingProvider", true},
            {"documentOnTypeFormattingProvider",
             llvm::json::Object{
                 {"firstTriggerCharacter", "}"},
                 {"moreTriggerCharacter", {}},
             }},
            {"codeActionProvider", true},
            {"completionProvider",
             llvm::json::Object{
                 {"resolveProvider", false},
                 // We do extra checks for '>' and ':' in completion to only
                 // trigger on '->' and '::'.
                 {"triggerCharacters", {".", ">", ":"}},
             }},
            {"signatureHelpProvider",
             llvm::json::Object{
                 {"triggerCharacters", {"(", ","}},
             }},
            {"definitionProvider", true},
            {"documentHighlightProvider", true},
            {"hoverProvider", true},
            {"renameProvider", true},
            {"documentSymbolProvider", true},
            {"workspaceSymbolProvider", true},
            {"referencesProvider", true},
            {"executeCommandProvider",
             llvm::json::Object{
                 {"commands", {ExecuteCommandParams::CLANGD_APPLY_FIX_COMMAND}},
             }},
        }}}});
}

void ClangdLSPServer::onShutdown(const ShutdownParams &Params,
                                 Callback<std::nullptr_t> Reply) {
  // Do essentially nothing, just say we're ready to exit.
  ShutdownRequestReceived = true;
  Reply(nullptr);
}

// sync is a clangd extension: it blocks until all background work completes.
// It blocks the calling thread, so no messages are processed until it returns!
void ClangdLSPServer::onSync(const NoParams &Params,
                             Callback<std::nullptr_t> Reply) {
  if (Server->blockUntilIdleForTest(/*TimeoutSeconds=*/60))
    Reply(nullptr);
  else
    Reply(llvm::createStringError(llvm::inconvertibleErrorCode(),
                                  "Not idle after a minute"));
}

void ClangdLSPServer::onDocumentDidOpen(
    const DidOpenTextDocumentParams &Params) {
  PathRef File = Params.textDocument.uri.file();

  const std::string &Contents = Params.textDocument.text;

  DraftMgr.addDraft(File, Contents);
  Server->addDocument(File, Contents, WantDiagnostics::Yes);
}

void ClangdLSPServer::onDocumentDidChange(
    const DidChangeTextDocumentParams &Params) {
  auto WantDiags = WantDiagnostics::Auto;
  if (Params.wantDiagnostics.hasValue())
    WantDiags = Params.wantDiagnostics.getValue() ? WantDiagnostics::Yes
                                                  : WantDiagnostics::No;

  PathRef File = Params.textDocument.uri.file();
  llvm::Expected<std::string> Contents =
      DraftMgr.updateDraft(File, Params.contentChanges);
  if (!Contents) {
    // If this fails, we are most likely going to be not in sync anymore with
    // the client.  It is better to remove the draft and let further operations
    // fail rather than giving wrong results.
    DraftMgr.removeDraft(File);
    Server->removeDocument(File);
    elog("Failed to update {0}: {1}", File, Contents.takeError());
    return;
  }

  Server->addDocument(File, *Contents, WantDiags);
}

void ClangdLSPServer::onFileEvent(const DidChangeWatchedFilesParams &Params) {
  Server->onFileEvent(Params);
}

void ClangdLSPServer::onCommand(const ExecuteCommandParams &Params,
                                Callback<llvm::json::Value> Reply) {
  auto ApplyEdit = [&](WorkspaceEdit WE) {
    ApplyWorkspaceEditParams Edit;
    Edit.edit = std::move(WE);
    // Ideally, we would wait for the response and if there is no error, we
    // would reply success/failure to the original RPC.
    call("workspace/applyEdit", Edit);
  };
  if (Params.command == ExecuteCommandParams::CLANGD_APPLY_FIX_COMMAND &&
      Params.workspaceEdit) {
    // The flow for "apply-fix" :
    // 1. We publish a diagnostic, including fixits
    // 2. The user clicks on the diagnostic, the editor asks us for code actions
    // 3. We send code actions, with the fixit embedded as context
    // 4. The user selects the fixit, the editor asks us to apply it
    // 5. We unwrap the changes and send them back to the editor
    // 6. The editor applies the changes (applyEdit), and sends us a reply (but
    // we ignore it)

    Reply("Fix applied.");
    ApplyEdit(*Params.workspaceEdit);
  } else {
    // We should not get here because ExecuteCommandParams would not have
    // parsed in the first place and this handler should not be called. But if
    // more commands are added, this will be here has a safe guard.
    Reply(llvm::make_error<LSPError>(
        llvm::formatv("Unsupported command \"{0}\".", Params.command).str(),
        ErrorCode::InvalidParams));
  }
}

void ClangdLSPServer::onWorkspaceSymbol(
    const WorkspaceSymbolParams &Params,
    Callback<std::vector<SymbolInformation>> Reply) {
  Server->workspaceSymbols(
      Params.query, CCOpts.Limit,
      Bind(
          [this](decltype(Reply) Reply,
                 llvm::Expected<std::vector<SymbolInformation>> Items) {
            if (!Items)
              return Reply(Items.takeError());
            for (auto &Sym : *Items)
              Sym.kind = adjustKindToCapability(Sym.kind, SupportedSymbolKinds);

            Reply(std::move(*Items));
          },
          std::move(Reply)));
}

void ClangdLSPServer::onRename(const RenameParams &Params,
                               Callback<WorkspaceEdit> Reply) {
  Path File = Params.textDocument.uri.file();
  llvm::Optional<std::string> Code = DraftMgr.getDraft(File);
  if (!Code)
    return Reply(llvm::make_error<LSPError>(
        "onRename called for non-added file", ErrorCode::InvalidParams));

  Server->rename(
      File, Params.position, Params.newName,
      Bind(
          [File, Code, Params](
              decltype(Reply) Reply,
              llvm::Expected<std::vector<tooling::Replacement>> Replacements) {
            if (!Replacements)
              return Reply(Replacements.takeError());

            // Turn the replacements into the format specified by the Language
            // Server Protocol. Fuse them into one big JSON array.
            std::vector<TextEdit> Edits;
            for (const auto &R : *Replacements)
              Edits.push_back(replacementToEdit(*Code, R));
            WorkspaceEdit WE;
            WE.changes = {{Params.textDocument.uri.uri(), Edits}};
            Reply(WE);
          },
          std::move(Reply)));
}

void ClangdLSPServer::onDocumentDidClose(
    const DidCloseTextDocumentParams &Params) {
  PathRef File = Params.textDocument.uri.file();
  DraftMgr.removeDraft(File);
  Server->removeDocument(File);
}

void ClangdLSPServer::onDocumentOnTypeFormatting(
    const DocumentOnTypeFormattingParams &Params,
    Callback<std::vector<TextEdit>> Reply) {
  auto File = Params.textDocument.uri.file();
  auto Code = DraftMgr.getDraft(File);
  if (!Code)
    return Reply(llvm::make_error<LSPError>(
        "onDocumentOnTypeFormatting called for non-added file",
        ErrorCode::InvalidParams));

  auto ReplacementsOrError = Server->formatOnType(*Code, File, Params.position);
  if (ReplacementsOrError)
    Reply(replacementsToEdits(*Code, ReplacementsOrError.get()));
  else
    Reply(ReplacementsOrError.takeError());
}

void ClangdLSPServer::onDocumentRangeFormatting(
    const DocumentRangeFormattingParams &Params,
    Callback<std::vector<TextEdit>> Reply) {
  auto File = Params.textDocument.uri.file();
  auto Code = DraftMgr.getDraft(File);
  if (!Code)
    return Reply(llvm::make_error<LSPError>(
        "onDocumentRangeFormatting called for non-added file",
        ErrorCode::InvalidParams));

  auto ReplacementsOrError = Server->formatRange(*Code, File, Params.range);
  if (ReplacementsOrError)
    Reply(replacementsToEdits(*Code, ReplacementsOrError.get()));
  else
    Reply(ReplacementsOrError.takeError());
}

void ClangdLSPServer::onDocumentFormatting(
    const DocumentFormattingParams &Params,
    Callback<std::vector<TextEdit>> Reply) {
  auto File = Params.textDocument.uri.file();
  auto Code = DraftMgr.getDraft(File);
  if (!Code)
    return Reply(llvm::make_error<LSPError>(
        "onDocumentFormatting called for non-added file",
        ErrorCode::InvalidParams));

  auto ReplacementsOrError = Server->formatFile(*Code, File);
  if (ReplacementsOrError)
    Reply(replacementsToEdits(*Code, ReplacementsOrError.get()));
  else
    Reply(ReplacementsOrError.takeError());
}

/// The functions constructs a flattened view of the DocumentSymbol hierarchy.
/// Used by the clients that do not support the hierarchical view.
static std::vector<SymbolInformation>
flattenSymbolHierarchy(llvm::ArrayRef<DocumentSymbol> Symbols,
                       const URIForFile &FileURI) {

  std::vector<SymbolInformation> Results;
  std::function<void(const DocumentSymbol &, llvm::StringRef)> Process =
      [&](const DocumentSymbol &S, llvm::Optional<llvm::StringRef> ParentName) {
        SymbolInformation SI;
        SI.containerName = ParentName ? "" : *ParentName;
        SI.name = S.name;
        SI.kind = S.kind;
        SI.location.range = S.range;
        SI.location.uri = FileURI;

        Results.push_back(std::move(SI));
        std::string FullName =
            !ParentName ? S.name : (ParentName->str() + "::" + S.name);
        for (auto &C : S.children)
          Process(C, /*ParentName=*/FullName);
      };
  for (auto &S : Symbols)
    Process(S, /*ParentName=*/"");
  return Results;
}

void ClangdLSPServer::onDocumentSymbol(const DocumentSymbolParams &Params,
                                       Callback<llvm::json::Value> Reply) {
  URIForFile FileURI = Params.textDocument.uri;
  Server->documentSymbols(
      Params.textDocument.uri.file(),
      Bind(
          [this, FileURI](decltype(Reply) Reply,
                          llvm::Expected<std::vector<DocumentSymbol>> Items) {
            if (!Items)
              return Reply(Items.takeError());
            adjustSymbolKinds(*Items, SupportedSymbolKinds);
            if (SupportsHierarchicalDocumentSymbol)
              return Reply(std::move(*Items));
            else
              return Reply(flattenSymbolHierarchy(*Items, FileURI));
          },
          std::move(Reply)));
}

static llvm::Optional<Command> asCommand(const CodeAction &Action) {
  Command Cmd;
  if (Action.command && Action.edit)
    return None; // Not representable. (We never emit these anyway).
  if (Action.command) {
    Cmd = *Action.command;
  } else if (Action.edit) {
    Cmd.command = Command::CLANGD_APPLY_FIX_COMMAND;
    Cmd.workspaceEdit = *Action.edit;
  } else {
    return None;
  }
  Cmd.title = Action.title;
  if (Action.kind && *Action.kind == CodeAction::QUICKFIX_KIND)
    Cmd.title = "Apply fix: " + Cmd.title;
  return Cmd;
}

void ClangdLSPServer::onCodeAction(const CodeActionParams &Params,
                                   Callback<llvm::json::Value> Reply) {
  auto Code = DraftMgr.getDraft(Params.textDocument.uri.file());
  if (!Code)
    return Reply(llvm::make_error<LSPError>(
        "onCodeAction called for non-added file", ErrorCode::InvalidParams));
  // We provide a code action for Fixes on the specified diagnostics.
  std::vector<CodeAction> Actions;
  for (const Diagnostic &D : Params.context.diagnostics) {
    for (auto &F : getFixes(Params.textDocument.uri.file(), D)) {
      Actions.push_back(toCodeAction(F, Params.textDocument.uri));
      Actions.back().diagnostics = {D};
    }
  }

  if (SupportsCodeAction)
    Reply(llvm::json::Array(Actions));
  else {
    std::vector<Command> Commands;
    for (const auto &Action : Actions)
      if (auto Command = asCommand(Action))
        Commands.push_back(std::move(*Command));
    Reply(llvm::json::Array(Commands));
  }
}

void ClangdLSPServer::onCompletion(const CompletionParams &Params,
                                   Callback<CompletionList> Reply) {
  if (!shouldRunCompletion(Params))
    return Reply(llvm::make_error<IgnoreCompletionError>());
  Server->codeComplete(Params.textDocument.uri.file(), Params.position, CCOpts,
                       Bind(
                           [this](decltype(Reply) Reply,
                                  llvm::Expected<CodeCompleteResult> List) {
                             if (!List)
                               return Reply(List.takeError());
                             CompletionList LSPList;
                             LSPList.isIncomplete = List->HasMore;
                             for (const auto &R : List->Completions) {
                               CompletionItem C = R.render(CCOpts);
                               C.kind = adjustKindToCapability(
                                   C.kind, SupportedCompletionItemKinds);
                               LSPList.items.push_back(std::move(C));
                             }
                             return Reply(std::move(LSPList));
                           },
                           std::move(Reply)));
}

void ClangdLSPServer::onSignatureHelp(const TextDocumentPositionParams &Params,
                                      Callback<SignatureHelp> Reply) {
  Server->signatureHelp(Params.textDocument.uri.file(), Params.position,
                        std::move(Reply));
}

void ClangdLSPServer::onGoToDefinition(const TextDocumentPositionParams &Params,
                                       Callback<std::vector<Location>> Reply) {
  Server->findDefinitions(Params.textDocument.uri.file(), Params.position,
                          std::move(Reply));
}

void ClangdLSPServer::onSwitchSourceHeader(const TextDocumentIdentifier &Params,
                                           Callback<std::string> Reply) {
  llvm::Optional<Path> Result = Server->switchSourceHeader(Params.uri.file());
  Reply(Result ? URI::createFile(*Result).toString() : "");
}

void ClangdLSPServer::onDocumentHighlight(
    const TextDocumentPositionParams &Params,
    Callback<std::vector<DocumentHighlight>> Reply) {
  Server->findDocumentHighlights(Params.textDocument.uri.file(),
                                 Params.position, std::move(Reply));
}

void ClangdLSPServer::onHover(const TextDocumentPositionParams &Params,
                              Callback<llvm::Optional<Hover>> Reply) {
  Server->findHover(Params.textDocument.uri.file(), Params.position,
                    std::move(Reply));
}

void ClangdLSPServer::applyConfiguration(
    const ConfigurationSettings &Settings) {
  // Per-file update to the compilation database.
  bool ShouldReparseOpenFiles = false;
  for (auto &Entry : Settings.compilationDatabaseChanges) {
    /// The opened files need to be reparsed only when some existing
    /// entries are changed.
    PathRef File = Entry.first;
    auto Old = CDB->getCompileCommand(File);
    auto New =
        tooling::CompileCommand(std::move(Entry.second.workingDirectory), File,
                                std::move(Entry.second.compilationCommand),
                                /*Output=*/"");
    if (Old != New) {
      CDB->setCompileCommand(File, std::move(New));
      ShouldReparseOpenFiles = true;
    }
  }
  if (ShouldReparseOpenFiles)
    reparseOpenedFiles();
}

// FIXME: This function needs to be properly tested.
void ClangdLSPServer::onChangeConfiguration(
    const DidChangeConfigurationParams &Params) {
  applyConfiguration(Params.settings);
}

void ClangdLSPServer::onReference(const ReferenceParams &Params,
                                  Callback<std::vector<Location>> Reply) {
  Server->findReferences(Params.textDocument.uri.file(), Params.position,
                         CCOpts.Limit, std::move(Reply));
}

void ClangdLSPServer::onSymbolInfo(const TextDocumentPositionParams &Params,
                                   Callback<std::vector<SymbolDetails>> Reply) {
  Server->symbolInfo(Params.textDocument.uri.file(), Params.position,
                     std::move(Reply));
}

ClangdLSPServer::ClangdLSPServer(class Transport &Transp,
                                 const clangd::CodeCompleteOptions &CCOpts,
                                 llvm::Optional<Path> CompileCommandsDir,
                                 bool UseDirBasedCDB,
                                 const ClangdServer::Options &Opts)
    : Transp(Transp), MsgHandler(new MessageHandler(*this)), CCOpts(CCOpts),
      SupportedSymbolKinds(defaultSymbolKinds()),
      SupportedCompletionItemKinds(defaultCompletionItemKinds()),
      UseDirBasedCDB(UseDirBasedCDB),
      CompileCommandsDir(std::move(CompileCommandsDir)),
      ClangdServerOpts(Opts) {
  // clang-format off
  MsgHandler->bind("initialize", &ClangdLSPServer::onInitialize);
  MsgHandler->bind("shutdown", &ClangdLSPServer::onShutdown);
  MsgHandler->bind("sync", &ClangdLSPServer::onSync);
  MsgHandler->bind("textDocument/rangeFormatting", &ClangdLSPServer::onDocumentRangeFormatting);
  MsgHandler->bind("textDocument/onTypeFormatting", &ClangdLSPServer::onDocumentOnTypeFormatting);
  MsgHandler->bind("textDocument/formatting", &ClangdLSPServer::onDocumentFormatting);
  MsgHandler->bind("textDocument/codeAction", &ClangdLSPServer::onCodeAction);
  MsgHandler->bind("textDocument/completion", &ClangdLSPServer::onCompletion);
  MsgHandler->bind("textDocument/signatureHelp", &ClangdLSPServer::onSignatureHelp);
  MsgHandler->bind("textDocument/definition", &ClangdLSPServer::onGoToDefinition);
  MsgHandler->bind("textDocument/references", &ClangdLSPServer::onReference);
  MsgHandler->bind("textDocument/switchSourceHeader", &ClangdLSPServer::onSwitchSourceHeader);
  MsgHandler->bind("textDocument/rename", &ClangdLSPServer::onRename);
  MsgHandler->bind("textDocument/hover", &ClangdLSPServer::onHover);
  MsgHandler->bind("textDocument/documentSymbol", &ClangdLSPServer::onDocumentSymbol);
  MsgHandler->bind("workspace/executeCommand", &ClangdLSPServer::onCommand);
  MsgHandler->bind("textDocument/documentHighlight", &ClangdLSPServer::onDocumentHighlight);
  MsgHandler->bind("workspace/symbol", &ClangdLSPServer::onWorkspaceSymbol);
  MsgHandler->bind("textDocument/didOpen", &ClangdLSPServer::onDocumentDidOpen);
  MsgHandler->bind("textDocument/didClose", &ClangdLSPServer::onDocumentDidClose);
  MsgHandler->bind("textDocument/didChange", &ClangdLSPServer::onDocumentDidChange);
  MsgHandler->bind("workspace/didChangeWatchedFiles", &ClangdLSPServer::onFileEvent);
  MsgHandler->bind("workspace/didChangeConfiguration", &ClangdLSPServer::onChangeConfiguration);
  MsgHandler->bind("textDocument/symbolInfo", &ClangdLSPServer::onSymbolInfo);
  // clang-format on
}

ClangdLSPServer::~ClangdLSPServer() = default;

bool ClangdLSPServer::run() {
  // Run the Language Server loop.
  bool CleanExit = true;
  if (auto Err = Transp.loop(*MsgHandler)) {
    elog("Transport error: {0}", std::move(Err));
    CleanExit = false;
  }

  // Destroy ClangdServer to ensure all worker threads finish.
  Server.reset();
  return CleanExit && ShutdownRequestReceived;
}

std::vector<Fix> ClangdLSPServer::getFixes(llvm::StringRef File,
                                           const clangd::Diagnostic &D) {
  std::lock_guard<std::mutex> Lock(FixItsMutex);
  auto DiagToFixItsIter = FixItsMap.find(File);
  if (DiagToFixItsIter == FixItsMap.end())
    return {};

  const auto &DiagToFixItsMap = DiagToFixItsIter->second;
  auto FixItsIter = DiagToFixItsMap.find(D);
  if (FixItsIter == DiagToFixItsMap.end())
    return {};

  return FixItsIter->second;
}

bool ClangdLSPServer::shouldRunCompletion(
    const CompletionParams &Params) const {
  llvm::StringRef Trigger = Params.context.triggerCharacter;
  if (Params.context.triggerKind != CompletionTriggerKind::TriggerCharacter ||
      (Trigger != ">" && Trigger != ":"))
    return true;

  auto Code = DraftMgr.getDraft(Params.textDocument.uri.file());
  if (!Code)
    return true; // completion code will log the error for untracked doc.

  // A completion request is sent when the user types '>' or ':', but we only
  // want to trigger on '->' and '::'. We check the preceeding character to make
  // sure it matches what we expected.
  // Running the lexer here would be more robust (e.g. we can detect comments
  // and avoid triggering completion there), but we choose to err on the side
  // of simplicity here.
  auto Offset = positionToOffset(*Code, Params.position,
                                 /*AllowColumnsBeyondLineLength=*/false);
  if (!Offset) {
    vlog("could not convert position '{0}' to offset for file '{1}'",
         Params.position, Params.textDocument.uri.file());
    return true;
  }
  if (*Offset < 2)
    return false;

  if (Trigger == ">")
    return (*Code)[*Offset - 2] == '-'; // trigger only on '->'.
  if (Trigger == ":")
    return (*Code)[*Offset - 2] == ':'; // trigger only on '::'.
  assert(false && "unhandled trigger character");
  return true;
}

void ClangdLSPServer::onDiagnosticsReady(PathRef File,
                                         std::vector<Diag> Diagnostics) {
  auto URI = URIForFile::canonicalize(File, /*TUPath=*/File);
  std::vector<Diagnostic> LSPDiagnostics;
  DiagnosticToReplacementMap LocalFixIts; // Temporary storage
  for (auto &Diag : Diagnostics) {
    toLSPDiags(Diag, URI, DiagOpts,
               [&](clangd::Diagnostic Diag, llvm::ArrayRef<Fix> Fixes) {
                 auto &FixItsForDiagnostic = LocalFixIts[Diag];
                 llvm::copy(Fixes, std::back_inserter(FixItsForDiagnostic));
                 LSPDiagnostics.push_back(std::move(Diag));
               });
  }

  // Cache FixIts
  {
    // FIXME(ibiryukov): should be deleted when documents are removed
    std::lock_guard<std::mutex> Lock(FixItsMutex);
    FixItsMap[File] = LocalFixIts;
  }

  // Publish diagnostics.
  notify("textDocument/publishDiagnostics",
         llvm::json::Object{
             {"uri", URI},
             {"diagnostics", std::move(LSPDiagnostics)},
         });
}

void ClangdLSPServer::onFileUpdated(PathRef File, const TUStatus &Status) {
  if (!SupportFileStatus)
    return;
  // FIXME: we don't emit "BuildingFile" and `RunningAction`, as these
  // two statuses are running faster in practice, which leads the UI constantly
  // changing, and doesn't provide much value. We may want to emit status at a
  // reasonable time interval (e.g. 0.5s).
  if (Status.Action.S == TUAction::BuildingFile ||
      Status.Action.S == TUAction::RunningAction)
    return;
  notify("textDocument/clangd.fileStatus", Status.render(File));
}

void ClangdLSPServer::reparseOpenedFiles() {
  for (const Path &FilePath : DraftMgr.getActiveFiles())
    Server->addDocument(FilePath, *DraftMgr.getDraft(FilePath),
                        WantDiagnostics::Auto);
}

} // namespace clangd
} // namespace clang
