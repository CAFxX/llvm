implementation

;; This is a simple nested loop to test interval construction

int "loop test"(int %i, int %j)
begin
Start:
	%j1 = add int 0, 0
	br label %L1Header

L1Header:
	%j2 = phi int [%j1, %Start], [%j3, %L2Done]

	%i1 = add int 0, 0             ; %i1 = 0
	br label %L2Body
L2Body:
	%i2 = phi int [%i1, %L1Header], [%i3, %L2Body]
	%i3 = add int %i2, 1
	%L2Done = seteq int %i3, 10
	br bool %L2Done, label %L2Done, label %L2Body
L2Done:
	%j3 = add int %j2, %i3
	%L1Done = seteq int %j3, 100
	br bool %L1Done, label %L1Done, label %L1Header

L1Done:
	ret int %j3
end

