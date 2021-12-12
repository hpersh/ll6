(setq os (new Module "../libs/os.so"))
(setq argsparse (new Module "../libs/argsparse.l"))

(new Metaclass 'Target Obj)

(atput Target.instance-methods '__init__
       (lambda (self name file deps cmd)
	 (self.name  := name)
	 (self.file  := file)
	 (self.deps  := deps)
	 (self.cmd   := cmd)
	 (self.phony := (not file))
	 )
       )


;; (atput Target.instance-methods 'repr
;;        (lambda (self)
;; 	 (format "<Target: {0}>" self.name)
;; 	 )
;;        )


;; (atput Target.instance-methods 'tostring
;;        (lambda (self)
;; 	 (repr self)
;; 	 )
;;        )


(atput Target.instance-methods 'run-cmd
       (lambda (self)
	 (if self.cmd
	     (progn
	       (print (format "Running command: {0}\n" self.cmd))
	       (if (not (equal (os.system self.cmd) 0))
		   (print "    FAILED!\n")
		   )
	       )
	     )
	 )
       )


(atput Target.instance-methods 'query
       (lambda (self dummy)
	 (print (format "Target {0}\n" self.name))
	 (if self.phony
	     (print "    Phony\n")
	     (print (format "    File: {0}\n" self.file))
	     )
	 (print "    Dependencies:\n        ")
	 (if self.deps
	     (foreach (lambda (d)
			(print (format "{0} "
				       (if (eq (instance-of d) Target) d.name d)
				       )
			       )
			)
		      self.deps		      
		      )
	     (print "None")
	     )
	 (print (format "\n    Command:\n        {0}\n" self.cmd))
	 )
       )


(atput Target.instance-methods 'build
       (lambda (self (debugf . nil))
	 (if debugf
	     (print (format "Building target {0}\n" self.name))
	     )
	 (setq mtime nil)
	 (setq rebuildf nil)
	 (if (not self.phony)
	     (progn
	       (setq st (stat File self.file))
	       (if (not st)
		   (progn
		     (setq rebuildf t)
		     (if debugf
			 (print "  Target does not exist\n")
			 )
		     )
		   (progn
		     (setq mtime st.st_mtime)
		     (if debugf
			 (progn
			   (print "  Target is not phony\n")
			   (print (format "  Mtime is {0}\n" mtime))
			   )
			 )
		     )
		   )
	       )
	     )
	 (foreach (lambda (dep)
		    (setq dep_mtime (cond
				      ((eq (instance-of dep) Target)
				       (if debugf
					   (print (format "  Processing dependency {0}\n" dep.name))
					   )
				       (build dep debugf)
				       )
				      ((eq (instance-of dep) String)
				       (if debugf
					   (print (format "  Processing dependency {0}\n" dep))
					   )
				       (setq s (stat File dep))
				       (if s
					   s.st_mtime
					   (print (format "Missing dependency {0}\n" dep))
					   nil
					   )
				       )
				      (t
				       (print (format "Bad dependency {0}, expecting a Target or String\n" dep))
				       nil
				       )
				      )
			  )
		    (if (not dep_mtime) (continue))
		    (if debugf
			(print (format "  Mtime is {0}\n" dep_mtime))
			)
		    (if (and mtime (gt dep_mtime mtime))
			(progn
			  (if debugf
			      (print "  Dependency is newer\n")
			      )
			  (setq rebuildf t)
			  )
			)
		    )
		  self.deps
		  )
	 (if self.phony
	     (progn
	       (run-cmd self)
	       nil
	       )
	     (if rebuildf
		 (run-cmd self)
		 )
	     (ate (stat File self.file) 'st_mtime)
	     )
	 )
       )


(def suffix-remove
    (lambda (s)
      (setq idx (rindex s "."))
      (if (not idx)
	  nil
	  (slice s 0 idx)
	  )
      )
  )


(def run
    (lambda (targets)
      (prog
	  (setq args
	   (enter argsparse
		  (parse
		   (new Dict
			(concat List
				(new Dptr 'options
				     (freeze (new Dict
						  (concat List
							  (new Dptr "-d"
							       (new Dict '((key . debug)
									   (doc . "Enable debug output")
									   )
								    )
							       )
							  )		   
						  )
					     )
				     )
				(new Dptr 'options-aliases
				     (freeze (new Dict '(("--debug" . "-d"))))
				     )
				(new Dptr 'positional
				     (concat List (freeze (new Dict '((key . cmd)
								      (doc . "Command")
								      (default . build)
								      )
							       )
							  )
					     )
				     )
				(new Dptr 'extra 'targets)
				(new Dptr 'help "Make")
				)
			)
		   (cdr argv)
		   )
		  )
	   )

	 ;; (print (format "{}\n" args))

	 (if (not args) (return))
	 
	 (args.cmd := (new Symbol args.cmd))

	 (if (equal args.cmd 'query)
	     (args.targets := (mapcar (lambda (x) (car x)) (new List targets)))
	     )
	 (if (not args.targets)
	     (args.targets := '("all"))
	     )
	 (foreach (lambda (x)
		    (setq y (at targets x))
		    (if (not y)
			(progn
			  (print (format "Unknown target: {0}\n" x))
			  (continue)
			  )
			)
		    (method-apply args.cmd (concat List (cdr y) args.debug))
		    )
		  args.targets		  
		  )
	 )
      )
  )

