// https://github.com/ajtulloch/quantcup-orderbook

#ifndef ENGINE_H_
#define ENGINE_H_

typedef unsigned long t_orderid;

/* Price
   0-65536 interpreted as divided by 100
   eg the range is 000.00-655.36
   eg the price 123.45 = 12345
   eg the price 23.45 = 2345
   eg the price 23.4 = 2340 */
typedef unsigned short t_price;
#define MAX_PRICE 65536
#define MIN_PRICE 1

/* Order Size */
typedef unsigned long t_size;

/* Side
   Ask=1, Bid=0 */
typedef int t_side;

/* Limit Order */
typedef struct {
  char *symbol;
  char *trader;
  t_side side;
  t_price price;
  t_size size;
} t_order;

/* Execution Report
   send one per opposite-sided order
   completely filled */
typedef t_order t_execution;

// EXTERNAL

/* IN:
   OUT: */
void init();

/* IN:
   OUT: */
void destroy();

/* IN: order: limit order to add to book
   OUT: orderid assigned to order
        start from 1 and increment with each call */
t_orderid limit(t_order order);

/* IN: orderid: id of order to cancel
   OUT:
   cancel request ignored if orderid not in book
*/
void cancel(t_orderid orderid);

// CALLBACKS

/* IN: execution: execution report
   OUT: */
void execution(t_execution exec);

#endif  // ENGINE_H_
