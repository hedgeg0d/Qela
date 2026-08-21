; tests.lisp — the lisp project's self-test, written in lisp itself.
;
; Run with the compiler's test runner, which checks the exit status
; declared in main.qela's comments:
;
;   qela test . tests.lisp

(define failed 0)

(define (check label got want)
  (if (equal? got want)
      (begin (display "ok   ") (display label) (newline))
      (begin (display "FAIL ") (display label)
             (display " got ") (write got)
             (display " want ") (write want)
             (newline)
             (set! failed (+ failed 1)))))

; arithmetic
(check "+" (+ 1 2 3) 6)
(check "-" (- 10 3 2) 5)
(check "*" (* 2 3 4) 24)
(check "/" (/ 17 5) 3)
(check "mod" (mod 17 5) 2)
(check "negate" (- 5) -5)

; lists
(check "car" (car '(1 2 3)) 1)
(check "cdr" (cdr '(1 2 3)) '(2 3))
(check "cons" (cons 1 '(2 3)) '(1 2 3))
(check "list" (list 1 2 3) '(1 2 3))
(check "append" (append '(1 2) '(3 4) '(5)) '(1 2 3 4 5))
(check "length" (length '(a b c)) 3)
(check "reverse" (reverse '(1 2 3)) '(3 2 1))
(check "list-ref" (list-ref '(10 20 30) 1) 20)
(check "dotted" (quote (a . b)) '(a . b))
(check "quote-nested" '(1 2 (3 4)) '(1 2 (3 4)))

; conditionals
(check "if" (if (> 3 2) 'yes 'no) 'yes)
(check "if-nil" (if nil 'no 'yes) 'yes)
(check "cond" (cond ((< 1 0) 'neg) ((> 1 0) 'pos) (else 'zero)) 'pos)
(check "and" (and 1 2 3) 3)
(check "and-short" (and 1 nil 3) nil)
(check "or" (or nil nil 7) 7)

; definitions and mutation
(define x 10)
(check "define" (* x x) 100)
(begin (define y 5) (set! y 6))
(check "set!" y 6)

; functions
(define (fact n) (if (< n 2) 1 (* n (fact (- n 1)))))
(check "fact" (fact 10) 3628800)
(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(check "fib" (fib 15) 610)
(check "lambda" ((lambda (x) (* x x)) 7) 49)
(check "let" (let ((a 3) (b 4)) (+ a b)) 7)
(check "let*" (let* ((a 2) (b (* a 10))) (+ a b)) 22)
(define (mk n) (lambda (x) (+ x n)))
(define add3 (mk 3))
(check "closure" (add3 10) 13)
(define (my-list . xs) xs)
(check "rest" (my-list 1 2 3) '(1 2 3))
(define (gcd a b) (if (= b 0) a (gcd b (mod a b))))
(check "gcd" (gcd 1071 462) 21)

; higher order
(check "map" (map (lambda (x) (* x x)) '(1 2 3 4)) '(1 4 9 16))
(check "apply" (apply + '(10 20 30)) 60)
(check "map-car" (map car '((1 2) (3 4))) '(1 3))

; loops
(define i 0)
(define acc '())
(while (< i 3) (set! acc (cons i acc)) (set! i (+ i 1)))
(check "while" (reverse acc) '(0 1 2))
(define sacc '())
(define si 10)
(while (> si 0) (set! sacc (cons si sacc)) (set! si (- si 1)))
(check "sum" (apply + sacc) 55)

; macros
(defmacro (twice x) (list 'begin x x))
(define counter 0)
(twice (set! counter (+ counter 1)))
(check "macro" counter 2)
(defmacro (unless c . body) (list 'if c nil (cons 'begin body)))
(define n 0)
(unless (= n 0) (set! n 1))
(check "unless" (+ n 100) 100)

; strings
(check "string-append" (string-append "ab" "cd" "e") "abcde")
(check "string-length" (string-length "hello") 5)
(check "string-ref" (string-ref "abc" 1) 98)
(check "string=?" (string=? "ab" "ab") 't)
(check "number->string" (number->string 42) "42")
(check "string->number" (string->number "-17") -17)
(check "string->number-bad" (string->number "nope") nil)

; predicates and equality
(check "not" (not nil) 't)
(check "eq-sym" (eq? 'a 'a) 't)
(check "eq-num" (eq? 3 3) 't)
(check "equal-nested" (equal? '(1 (2 3)) '(1 (2 3))) 't)
(check "equal-str" (equal? "ab" "ab") 't)
(check "null?" (null? '()) 't)
(check "pair?" (pair? '(1 2)) 't)
(check "list?" (list? '(1 2)) 't)
(check "symbol?" (symbol? 'foo) 't)
(check "number?" (number? 42) 't)
(check "procedure?" (procedure? car) 't)

(if (> failed 0) (exit 1) (exit 0))
