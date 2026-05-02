.PHONY: build run-recorder install uninstall clean cmake check lint tidy tidy-fix test help

help:
	@echo "使用可能なコマンド:"
	@echo "  make build          - ビルドのみ（インストール・署名なし）"
	@echo "  make install        - /Library にインストール（sudo + DawCast Dev 署名）"
	@echo "  make uninstall      - /Library からVST3/AUを削除"
	@echo "  make run-recorder   - ビルドして録画アプリを起動"
	@echo "  make cmake          - CMakeプロジェクトを再生成"
	@echo "  make check          - コンパイルをチェック（エラーのみ表示）"
	@echo "  make lint           - コンパイラ警告をチェック"
	@echo "  make test           - ユニットテストをビルド＆実行"
	@echo "  make coverage       - HTMLカバレッジレポートを生成してブラウザで開く"
	@echo "  make clean          - ビルドディレクトリをクリーン"

build:
	cd build && xcodebuild -scheme "DawCastPlugin_All" -configuration Debug build
	cd build && xcodebuild -scheme "DawCastRecorder" -configuration Debug build

run-recorder: build
	open build/DawCastRecorder_artefacts/Debug/DawCastRecorder.app

install: build
	@echo "→ VST3 を /Library にインストール中... (sudo が必要です)"
	sudo /usr/bin/rsync -a --delete --checksum \
		build/DawCastPlugin_artefacts/Debug/VST3/DawCast.vst3/ \
		/Library/Audio/Plug-Ins/VST3/DawCast.vst3/
	# DawCastRecorder.app を "DawCast Dev" 署名（TCC が証明書アンカー + Bundle ID で追跡）
	sudo /usr/bin/codesign --force --sign "DawCast Dev" \
		/Library/Audio/Plug-Ins/VST3/DawCast.vst3/Contents/Resources/DawCastRecorder.app
	# VST3 バンドル自体は ad-hoc 署名（--deep 禁止: Recorder.app の TCC がリセットされる）
	sudo /usr/bin/codesign --force --sign - \
		/Library/Audio/Plug-Ins/VST3/DawCast.vst3
	@echo "→ AU を /Library にインストール中..."
	sudo /usr/bin/rsync -a --delete --checksum \
		build/DawCastPlugin_artefacts/Debug/AU/DawCast.component/ \
		/Library/Audio/Plug-Ins/Components/DawCast.component/
	sudo /usr/bin/codesign --force --sign "DawCast Dev" \
		/Library/Audio/Plug-Ins/Components/DawCast.component/Contents/Resources/DawCastRecorder.app
	sudo /usr/bin/codesign --force --sign - \
		/Library/Audio/Plug-Ins/Components/DawCast.component
	@echo "✓ インストール完了。DAWで再スキャンしてください。"

uninstall:
	@echo "→ VST3/AU を /Library から削除中... (sudo が必要です)"
	sudo rm -rf /Library/Audio/Plug-Ins/VST3/DawCast.vst3
	sudo rm -rf /Library/Audio/Plug-Ins/Components/DawCast.component
	@echo "✓ アンインストール完了。"

cmake:
	cd build && cmake .. -G Xcode
	cd build-clangd && cmake ..

clean:
	rm -rf build/* build-clangd/*

check:
	cd build && cmake .. -G Xcode 2>/dev/null && \
	  xcodebuild -project DawCast.xcodeproj -scheme "DawCastPlugin_All" -configuration Debug build 2>&1 | grep -E "(error|warning):" || echo "✓ ビルドエラー・ワーニングなし"

lint:
	@echo "基本的なコード検査（コンパイラ警告）..."
	@cd build && xcodebuild -project DawCast.xcodeproj -scheme "DawCastPlugin_All" -configuration Debug build 2>&1 | \
		grep -E "(warning|error):" | \
		grep -v "Run script build phase" | \
		head -50 || echo "✓ 警告・エラーなし"

tidy:
	@echo "注意: clang-tidy は JUCE プロジェクトでは正常に動作しない可能性があります。"
	@echo "代わりに 'make lint' または 'make check' を使用してください。"
	@echo ""
	@echo "それでも実行する場合は以下を手動実行:"
	@echo "  /opt/homebrew/opt/llvm/bin/clang-tidy -p build-clangd Source/YourFile.cpp"

tidy-fix:
	@echo "clang-tidy の自動修正は JUCE プロジェクトでは推奨されません。"
	@echo "手動でコードを修正してください。"

test:
	cd build && xcodebuild -scheme "DawCastTests" -configuration Debug build 2>&1 | grep -E "(error:|Build succeeded|FAILED)" || true
	@cd build && LLVM_PROFILE_FILE=cov_%p.profraw ctest -C Debug --output-on-failure
	@echo ""
	@echo "── Coverage Report ──────────────────────────────────"
	@cd build && xcrun llvm-profdata merge -sparse cov_*.profraw -o cov.profdata 2>/dev/null && \
	  xcrun llvm-cov report ./DawCastTests_artefacts/Debug/DawCastTests \
	    -instr-profile=cov.profdata \
	    -ignore-filename-regex='(JUCE|Catch2|_deps|Tests/|GUI/|PluginEditor)' && \
	  rm -f cov_*.profraw || echo "(カバレッジデータなし)"

coverage:
	@echo "テストを実行してカバレッジデータを収集中..."
	@cd build && xcodebuild -scheme "DawCastTests" -configuration Debug build 2>&1 | grep -E "(error:|Build succeeded|FAILED)" || true
	@cd build && LLVM_PROFILE_FILE=cov_%p.profraw ctest -C Debug -Q
	@echo "HTMLレポートを生成中..."
	@cd build && xcrun llvm-profdata merge -sparse cov_*.profraw -o cov.profdata 2>/dev/null && \
	  xcrun llvm-cov show ./DawCastTests_artefacts/Debug/DawCastTests \
	    -instr-profile=cov.profdata \
	    -ignore-filename-regex='(JUCE|Catch2|_deps|Tests/|GUI/|PluginEditor)' \
	    -format=html \
	    -output-dir=../coverage-report \
	    -show-line-counts-or-regions \
	    -show-instantiations=false && \
	  rm -f cov_*.profraw cov.profdata && \
	  echo "✓ レポート生成完了: coverage-report/index.html" && \
	  open ../coverage-report/index.html || echo "(カバレッジデータなし)"
