// lexer: faithful port of bench/lexer.pas
#[derive(Clone, Copy, PartialEq)]
enum TKind { Ident, Keyword, Number, Str, Symbol, Comment }

const SNIPPETS: [&str; 8] = [
    "program demo; var count, total: integer; ",
    "begin count := 0; total := 42 * (count + 7); ",
    "if total > 100 then writeln('big') else writeln('small'); ",
    "{ a comment } while count < 10 do begin count := count + 1 end; ",
    "for i := 1 to 99 do total := total div 3 + i mod 5; ",
    "case total of 1: x := 2; 3..9: y := x; else z := 0 end; ",
    "repeat total := total - 1 until total <= 0; ",
    "procedure p(a: string; var b: real); begin b := 3.25 end; end.",
];

fn keyword_code(word: &[u8]) -> i64 {
    if word.is_empty() { return 0; }
    match word[0] {
        b'b' if word == b"begin" => 3,
        b'c' if word == b"case" => 12,
        b'd' => {
            if word == b"do" { 9 }
            else if word == b"div" { 17 }
            else { 0 }
        }
        b'e' => {
            if word == b"end" { 4 }
            else if word == b"else" { 7 }
            else { 0 }
        }
        b'f' if word == b"for" => 10,
        b'i' if word == b"if" => 5,
        b'm' if word == b"mod" => 18,
        b'o' if word == b"of" => 13,
        b'p' => {
            if word == b"program" { 1 }
            else if word == b"procedure" { 16 }
            else { 0 }
        }
        b'r' if word == b"repeat" => 14,
        b't' => {
            if word == b"then" { 6 }
            else if word == b"to" { 11 }
            else { 0 }
        }
        b'u' if word == b"until" => 15,
        b'v' if word == b"var" => 2,
        b'w' if word == b"while" => 8,
        _ => 0,
    }
}

fn scan(text: &[u8], counts: &mut [i64; 6], keyword_hash: &mut i64) {
    let n = text.len();
    let mut i = 0usize; // i is 0-based; text[i] corresponds to Pascal's text[i+1]
    while i < n {
        let c = text[i];
        if c == b' ' || c == 9 || c == 10 || c == 13 {
            i += 1;
        } else if c.is_ascii_alphabetic() || c == b'_' {
            let start = i;
            while i < n {
                let c1 = text[i];
                if !(c1.is_ascii_alphanumeric() || c1 == b'_') { break; }
                i += 1;
            }
            let word = &text[start..i];
            let code = keyword_code(word);
            if code > 0 {
                counts[TKind::Keyword as usize] += 1;
                *keyword_hash = (*keyword_hash * 31 + code) % 1000003;
            } else {
                counts[TKind::Ident as usize] += 1;
            }
        } else if c.is_ascii_digit() {
            while i < n {
                let c1 = text[i];
                if !(c1.is_ascii_digit() || c1 == b'.') { break; }
                i += 1;
            }
            counts[TKind::Number as usize] += 1;
        } else if c == b'\'' {
            i += 1;
            while i < n && text[i] != b'\'' { i += 1; }
            i += 1;
            counts[TKind::Str as usize] += 1;
        } else if c == b'{' {
            while i < n && text[i] != b'}' { i += 1; }
            i += 1;
            counts[TKind::Comment as usize] += 1;
        } else if (i < n - 1)
            && (c == b':' || c == b'<' || c == b'>' || c == b'.')
            && (text[i + 1] == b'=' || text[i + 1] == b'>' || text[i + 1] == b'.')
        {
            i += 2;
            counts[TKind::Symbol as usize] += 1;
        } else {
            i += 1;
            counts[TKind::Symbol as usize] += 1;
        }
    }
}

fn main() {
    let mut counts = [0i64; 6];
    let mut keyword_hash: i64 = 0;
    let mut text = String::new();
    for i in 0..400 {
        text.push_str(SNIPPETS[(i * 5) % 8]);
    }
    let text = text.into_bytes();
    for _ in 1..=1500 {
        scan(&text, &mut counts, &mut keyword_hash);
    }
    let names = [TKind::Ident, TKind::Keyword, TKind::Number, TKind::Str, TKind::Symbol, TKind::Comment];
    for k in 0..6 {
        print!("{} ", counts[name_index(k)]);
    }
    println!();
    println!("{}", keyword_hash);
}

fn name_index(k: usize) -> usize { k }
