// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CARBON_TOOLCHAIN_LANGUAGE_SERVER_HANDLE_H_
#define CARBON_TOOLCHAIN_LANGUAGE_SERVER_HANDLE_H_

#include "toolchain/language_server/context.h"

namespace Carbon::LanguageServer {

// Stores the content of newly-opened documents.
auto HandleDidChangeTextDocument(
    Context& context, const clang::clangd::DidChangeTextDocumentParams& params)
    -> void;

// Updates the content of already-open documents.
auto HandleDidOpenTextDocument(
    Context& context, const clang::clangd::DidOpenTextDocumentParams& params)
    -> void;

// Provides information about document symbols.
auto HandleDocumentSymbol(
    Context& context, const clang::clangd::DocumentSymbolParams& params,
    llvm::function_ref<
        void(llvm::Expected<std::vector<clang::clangd::DocumentSymbol>>)>
        on_done) -> void;

// Tells the client what features are supported.
auto HandleInitialize(
    Context& /*context*/,
    const clang::clangd::NoParams& /*client_capabilities*/,
    llvm::function_ref<void(llvm::Expected<llvm::json::Object>)> on_done)
    -> void;

}  // namespace Carbon::LanguageServer

#endif  // CARBON_TOOLCHAIN_LANGUAGE_SERVER_HANDLE_H_
