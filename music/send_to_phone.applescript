-- "Send to Phone": drop music files or folders on the icon. Copies them to the iPhone and plays the first one.
property scriptPath : "__SCRIPT_PATH__"

on open theItems
	set args to ""
	repeat with f in theItems
		set args to args & " " & quoted form of POSIX path of f
	end repeat
	try
		do shell script "/usr/bin/python3 " & quoted form of scriptPath & args & " >> /tmp/send_to_phone.log 2>&1"
	on error msg
		display notification msg with title "Send to Phone"
	end try
end open

on run
	display dialog "Drag a music file or a folder of music onto this app's icon. It copies to the iPhone and plays." buttons {"OK"} default button "OK"
end run
