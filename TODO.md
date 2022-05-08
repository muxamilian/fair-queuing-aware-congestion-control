* ~Send fixed ratio of packets on path A and rest of packets on path B~
* ~Implement "meta congestion control": one cwnd for both paths~
  * ~Check that slow start is ok as well...~
* ~Implement the alternating ratio of packets on path A and path B~
* ~Implement that with Tonopah the sum of both cwnds counts and that the rate is calculated based on the cwnd for each path~
* ~Randomize pacing to make sure that not one path dominates~
* ~Test with cross traffic~
* ~Implement congestion window decrease on detection of FQ~
* Test how often it detects FQ (confusion matrix)
* Measure how much it can reduce latency
* Write paper