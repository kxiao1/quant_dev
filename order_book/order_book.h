#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

// Public structs

struct OrderState {
    int filledSize{-1};  // indicates that the order is invalid
    double averagePrice{0.0};
};

struct PriceLevel {
    int price{-1};  // indicates that there is no bid or ask
    int totalSize{-1};
};

struct L1_Data {
    PriceLevel bestBid;
    PriceLevel bestOffer;
};

struct L2_Data {
    std::vector<PriceLevel> bids;    // decreasing
    std::vector<PriceLevel> offers;  // increasing
};

// Private structs
struct LimitOrder {
    int price{0};
    int originalSize{0};
    int remainingSize{0};
    int filledValue{0};  // sum over price of price * qty filled at that price
};

struct OrderLevel {
    std::list<LimitOrder> orders;
    int totalSize{0};  // 0 = empty and potentially uninitialized, can skip
    int nextIdx{-1};   // index of next higher bid or lower offer
    int prevIdx{-1};
};

class OrderBook {
   public:
    OrderBook(int maxPrice, int increment);
    virtual ~OrderBook() = default;  // virtual destructor

    // Adds a new order. Returns true iff parameters are valid and a new orderId
    // that can be used to query its state. If the (aggressive) order is filled
    // immediately, a valid orderId is still returned.
    std::pair<bool, int> addOrder(int price, int orderSize, bool isBid);

    // Queries the state of an order without modifying it. The first return
    // value is true iff order is active (i.e. exists and not cancelled or fully
    // filled) and the second is the state of the order as defined above, if it
    // is active or fully filled.
    std::pair<bool, OrderState> getOrderStatus(int orderId);

    // Cancels unfilled part of existing order. The first return value is true
    // iff the operation succeeds, which is when the order is valid. The second
    // return value is the order's state right before it was cancelled.
    std::pair<bool, OrderState> cancelOrder(int orderId);

    // Changes an active order's price and/or amount (but not side). The first
    // return value is true iff the operation succeeds (see below) and the
    // second is the order state corresponding to the filled part if any.

    // If newPrice = old price, we set originalSize = newSize and remainingSize
    // = newSize - filled amount in the existing order and do not change its
    // priority. Otherwise, this function cancels the current order and enters a
    // new order with originalSize = remainingSize = newSize - filled amount.
    // NOP and status = false if remainingSize <= 0.
    std::pair<bool, OrderState> updateOrder(int orderId, int newPrice,
                                            int newSize);

    L1_Data getL1OrderData();
    L2_Data getL2OrderData();

   private:
    const int maxP, incr;  // max price and the price increment per index
    int nextOrderId{1};    // next order id is incremented by 1 each time

    // A sparse vector of all possible price levels with filled levels linked
    // together by prev and next pointers. First and Last bids and offers are
    // marked as with linked lists for ease of iteration. idx = -1 <=> end().
    std::vector<OrderLevel> orderLevels;
    int firstBidIdx{-1}, lastBidIdx{-1};      // last bid = highest bid
    int firstOfferIdx{-1}, lastOfferIdx{-1};  // last offer = lowest offer

    // Maps orderId to Order object. We can use a vector, but then we have to
    // indicate cancelled orders (std::optional?) because invalidated iterators
    // cannot be safely dereferenced. We can't use a vector<LimitOrder*> either,
    // because we are manipulating a STL list rather than a handcrafted list.
    // https://stackoverflow.com/questions/2062956/checking-if-an-iterator-is-valid
    std::unordered_map<int, std::list<LimitOrder>::iterator> activeOrderMap;

    // Stores the amount and average price of completed orders. This map
    // excludes cancelled orders and the filled part of orders with price
    // updates. We need to store completed orders in another data structure
    // because we can't make a STL list skip done orders without also destroying
    // them or manually pointing to the head of the list (a drawback of
    // automatic memory management?). With a handcrafted list we can just move
    // the head pointer accordingly and delete all done orders when we need to
    // manage memory.
    std::unordered_map<int, OrderState> doneOrderMap;

    // Fills orders at currIdx up to orderSize. Returns next index to check if
    // the current idx is exhausted, otherwise returns the current idx.
    std::pair<int, int> fillOrdersAtCurrIdx(int currIdx, int orderSize);

    // Creates new bid or offer level at the provided newIdx.
    void addNewOrderLevel(int newIdx, bool isBid);

    // Removes an existing level indicated by currIdx. Because bids and offers
    // never cross, whether an existing level is a bid or offer level can be
    // internally determined by calling getIsBid.
    void removeOrderLevel(int currIdx);

    // Determines if an existing level is a bid or offer level.
    bool getIsBid(int currIdx);

    // Determines if price and orderSize are valid
    bool getIsOrderValid(int price, int orderSize);
};

inline bool OrderBook::getIsBid(int currIdx) {
    // If lastBidIdx >= 0, then a bid is always less than last bid thus true
    // If lastBidIdx < 0, there were no bids to start thus false
    return currIdx <= lastBidIdx;
}

inline bool OrderBook::getIsOrderValid(int price, int orderSize) {
    return (price >= 0) && (price <= maxP) && (price % incr == 0) &&
           (orderSize > 0);
}
