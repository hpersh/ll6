
;; Returns a Dict, with (argument . value) pairs

;; Input
;;
;; { options: { <arg>: { key: <key>,
;;                       param: <bool>,
;;                       default: <default-value>,
;;                       doc: <docstring>
;;                     },
;;              ...
;;            }
;;   options-aliases: { <alias>: <arg, ... }
;;   positional: ( { key: <key>,
;;                   default: <default-value>,
;;                   doc: <docstring>
;;                 }
;;               ...
;;               )
;;   extra: <key>
;;   help: <docstring>
;; }

;; Output
;;
;; { <key>: <value>, ... }, or nil if error

(def print-help
    (lambda (cfg)
      (print (format "{}\n" cfg.help))
      (print "SYNTAX:\n")
      (setq aliases-reverse-dict (new Dict))
      (foreach (lambda (x) 
		 (setq k (cdr x))
		 (setq v (at-default aliases-reverse-dict k ""))
		 (atput aliases-reverse-dict k
			(format "{}{}{}" v (if (equal v "") "" ",") (car x))
			)
		 )
	       (at-default cfg 'options-aliases nil)
	       )
      (foreach (lambda (opt)
		 (setq aliases (at-default aliases-reverse-dict (car opt) ""))
		 (print (format "{}{}{}\t{}\n"
				(car opt)
				(if (equal aliases "") "" ",")
				aliases
				(ate (cdr opt) 'doc)
				)
			)
		 )
	       (sort (new Array (at-default cfg 'options nil))
			 (lambda (a b) (cmp (car a) (car b)))
			 )
	       )
      (foreach (lambda (arg)
		 (print (format "{}\t{}\n" arg.key arg.doc))
		 (print (format "\t{}\n"
				(if (setq x (at arg 'default))
				    (format "Default is {}" (cdr x))
				    "Required"
				    )
				)
			)
		 )
	       (at-default cfg 'positional nil)
	       )
      (if (at cfg 'extra)
	  (print "...\n")
	  )
      )
  )

(def parse
    (lambda (cfg argv)
      (prog
	  (setq result (new Dict))
	 (setq pl (at-default cfg 'positional nil))
	 (setq opts-got (new Set))

	 (while argv
	   (setq arg (car argv))
	   (setq argv (cdr argv))
	   (if (equal (slice arg 0 1) "-")
	       ;; Argument is optional
	       
	       (progn
		 ;; Translate any alias
		 
		 (if (and (setq x (at cfg 'options-aliases))
			  (setq y (at (cdr x) arg))
			  )
		     (setq arg (cdr y))
		     )

		 (setq opt-arg (and (setq x (at cfg 'options))
				    (at (cdr x) arg)
				    )
		       )
		 (if (not opt-arg)
		     ;; Option not defined
		     
		     (progn
		       (print-help cfg)
		       (return nil)
		       )
		     )
		 
		 (setq opt-arg (cdr opt-arg))
		 (if (at opt-arg 'param)
		     ;; Option takes a parameter
		     
		     (progn
		       (if (not argv)
			   ;; No parameter given
			   
			   (progn
			     (print-help cfg)
			     (return nil)
			     )
			   )

		       ;; Add option with parameter to result
		       
		       (atput result opt-arg.key (car argv))
		       (setq argv (cdr argv))
		       )

		     ;; Add option with no parameter to result
		     
		     (atput result opt-arg.key t)
		     )

		 ;; Mark option as given
		 
		 (put opts-got arg)
		 
		 (continue)
		 )
	       )

	   ;; Argument is positional
	   
	   (if (not pl)
	       ;; No more positional args are expected
	       ;; => Add it to the extras, if extras are allowed
	       
	       (progn
		 (setq x (at cfg 'extra))
		 (if (not x)
		     (progn
		       (print-help cfg)
		       (return nil)
		       )
		     )
		 (setq x (cdr x))
		 (atput result x (append (at-default result x nil) arg))
		 (continue)
		 )
	       )

	   ;; Add positional to result

	   (atput result (ate (car pl) 'key) arg)
	   (setq pl (cdr pl))
	   )

	 ;; For each remaining positional arg, add its default value
	 ;; to the result; if a positional does not have a default
	 ;; value, it's an error

	 (foreach (lambda (arg)
		    (if (not (setq x (at arg 'default)))
			(progn
			  (print-help cfg)
			  (return nil)
			  )
			)
		    (atput result arg.key (cdr x))
		    )
		  pl
		  )

	 ;; If extra positional args are allowed, but none are given,
	 ;; show it

	 (if (setq x (at cfg 'extra))
	     (progn
	       (setq k (cdr x))
	       (if (not (at result k))
		   (atput result k nil)
		   )
	       )
	     )

	 ;; For each option that was not given, add its default value
	 ;; to the result

	 (foreach (lambda (opt-arg)
		    (if (at opts-got (car opt-arg))
			(continue)
			)
		    (setq opt-arg (cdr opt-arg))
		    (atput result opt-arg.key (if (at opt-arg 'param)
						  opt-arg.default
						  nil
						  )
			   )
		    )
		  (at-default cfg 'options nil)
		  )
	 )
      result
      )
  )

