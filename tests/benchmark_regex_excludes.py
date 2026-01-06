#!/usr/bin/env python3
"""Benchmark script for regex with excludes performance."""

import time
import statistics
import xgrammar as xgr
from transformers import AutoTokenizer

def benchmark_compile(pattern: str, excludes: list, iterations: int = 10):
    """Benchmark grammar compilation time."""
    stag = {'type': 'structural_tag', 'format': {'type': 'regex', 'pattern': pattern, 'excludes': excludes}}
    
    times = []
    for _ in range(iterations):
        start = time.perf_counter()
        grammar = xgr.Grammar.from_structural_tag(stag)
        times.append((time.perf_counter() - start) * 1000)
    
    grammar_str = str(grammar)
    rule_count = grammar_str.count('::=')
    
    return {
        'mean_ms': statistics.mean(times),
        'std_ms': statistics.stdev(times) if len(times) > 1 else 0,
        'min_ms': min(times),
        'max_ms': max(times),
        'rules': rule_count,
    }

def benchmark_matching(tokenizer_info, pattern: str, excludes: list, test_string: str):
    """Benchmark matching performance."""
    stag = {'type': 'structural_tag', 'format': {'type': 'regex', 'pattern': pattern, 'excludes': excludes}}
    compiler = xgr.GrammarCompiler(tokenizer_info, cache_enabled=False)
    compiled = compiler.compile_structural_tag(stag)
    
    # Match
    matcher = xgr.GrammarMatcher(compiled)
    token_bitmask = xgr.allocate_token_bitmask(1, tokenizer_info.vocab_size)
    
    match_times = []
    for char in test_string:
        matcher.accept_string(char)
        start = time.perf_counter()
        matcher.fill_next_token_bitmask(token_bitmask)
        match_times.append((time.perf_counter() - start) * 1000000)  # microseconds
    
    return statistics.mean(match_times) if match_times else 0

def run_benchmarks():
    """Run all benchmark cases."""
    cases = [
        # (label, pattern, excludes)
        ("Baseline: no excludes", "[a-z]+", []),
        ("1 short exclude", "[a-z]+", ["bad"]),
        ("3 short excludes", "[a-z]+", ["foo", "bar", "baz"]),
        ("1 long exclude (10)", "[a-z]+", ["a" * 10]),
        ("1 long exclude (20)", "[a-z]+", ["a" * 20]),
        ("5 medium excludes", "[a-z]+", ["hello", "world", "test", "debug", "error"]),
        ("10 short excludes", "[a-z]+", ["ab", "bc", "cd", "de", "ef", "fg", "gh", "hi", "ij", "jk"]),
        ("Complex regex", "[a-zA-Z_][a-zA-Z0-9_]*", ["function", "return", "class"]),
        ("Complex regex + 7 excludes", "[a-zA-Z_][a-zA-Z0-9_]*", 
         ["function", "return", "class", "if", "else", "while", "for"]),
    ]
    
    print("=" * 80)
    print("REGEX EXCLUDES BENCHMARK - COMPILE TIME")
    print("=" * 80)
    print(f"{'Case':<35} {'Mean (ms)':<12} {'Std':<10} {'Rules':<8}")
    print("-" * 80)
    
    results = []
    for label, pattern, excludes in cases:
        result = benchmark_compile(pattern, excludes)
        results.append((label, result))
        print(f"{label:<35} {result['mean_ms']:<12.3f} {result['std_ms']:<10.3f} {result['rules']:<8}")
    
    print("=" * 80)
    
    # Calculate improvement vs baseline
    baseline = results[0][1]['mean_ms']
    print("\nRelative to baseline:")
    for label, result in results[1:]:
        ratio = result['mean_ms'] / baseline
        print(f"  {label}: {ratio:.1f}x slower")

    # Matching benchmark
    print("\n" + "=" * 80)
    print("MATCHING PERFORMANCE (avg us per char)")
    print("=" * 80)
    
    try:
        tokenizer = AutoTokenizer.from_pretrained('meta-llama/Llama-3.1-8B-Instruct', use_fast=True)
        tokenizer_info = xgr.TokenizerInfo.from_huggingface(tokenizer)
        
        match_cases = [
            ("Baseline: no excludes", "[a-z]+", [], "helloworld"),
            ("1 short exclude", "[a-z]+", ["bad"], "helloworld"),
            ("3 short excludes", "[a-z]+", ["foo", "bar", "baz"], "helloworld"),
            ("Complex regex + 7 excludes", "[a-zA-Z_][a-zA-Z0-9_]*", 
             ["function", "return", "class", "if", "else", "while", "for"], "myVariable"),
        ]
        
        for label, pattern, excludes, test_string in match_cases:
            avg_match_us = benchmark_matching(tokenizer_info, pattern, excludes, test_string)
            print(f"  {label}: {avg_match_us:.1f} us/char")
    except Exception as e:
        print(f"  Matching benchmark skipped: {e}")
    
    return results

if __name__ == "__main__":
    run_benchmarks()

