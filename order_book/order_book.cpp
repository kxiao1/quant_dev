#include "order_book.h"

/* Public members*/

OrderBook::OrderBook(int maxPrice, int increment)
    : maxP(maxPrice), incr(increment) {
    if (maxPrice % increment != 0) {
        throw "maxPrice must be divisible by increment";
    }
    orderLevels.resize(maxPrice / increment + 1);  // value initialization
}

std::pair<bool, int> OrderBook::addOrder(int price, int orderSize, bool isBid) {
    // Check order parameters
    if (!getIsOrderValid(price, orderSize)) {
        return {false, -1};
    }

    int originalSize = orderSize;
    int newIdx = price / incr;
    int filledValue = 0;

    if (isBid) {
        int currOfferIdx = lastOfferIdx;
        while (currOfferIdx >= 0 && currOfferIdx <= newIdx && orderSize > 0) {
            auto [newOfferIdx, newOrderSize] =
                fillOrdersAtCurrIdx(currOfferIdx, orderSize);
            filledValue += (orderSize - newOrderSize) * currOfferIdx * incr;
            currOfferIdx = newOfferIdx;
            orderSize = newOrderSize;
        }
        lastOfferIdx = currOfferIdx;
        if (currOfferIdx < 0) {  // Highest offer taken, none remain
            firstOfferIdx = currOfferIdx;
        }
    } else {  // symmetrical for offers
        int currBidIdx = lastBidIdx;
        while (currBidIdx >= 0 && currBidIdx >= newIdx && orderSize > 0) {
            auto [newBidIdx, newOrderSize] =
                fillOrdersAtCurrIdx(currBidIdx, orderSize);
            filledValue += (orderSize - newOrderSize) * currBidIdx * incr;
            currBidIdx = newBidIdx;
            orderSize = newOrderSize;
        }
        lastBidIdx = currBidIdx;
        if (currBidIdx < 0) {  // Lowest bid given, none remain
            firstBidIdx = currBidIdx;
        }
    }

    // New order could have been instantly filled
    if (orderSize == 0) {
        // To be consistent with how we treat resting orders that are filled
        doneOrderMap.try_emplace(nextOrderId, originalSize,
                       static_cast<double>(filledValue) / originalSize);
        return {true, nextOrderId++};
    }

    // Initialize the order level if needed
    if (orderLevels[newIdx].totalSize == 0) {
        // orderLevels[newIdx].orders.clear();  // should be already empty
        addNewOrderLevel(newIdx, isBid);
    }

    // FIFO: always insert at the end of the order level
    auto it = orderLevels[newIdx].orders.insert(
        orderLevels[newIdx].orders.end(),
        {price, originalSize, orderSize, filledValue});
    orderLevels[newIdx].totalSize += orderSize;
    activeOrderMap.insert_or_assign(nextOrderId, std::move(it));  // insert into hash map

    return {true, nextOrderId++};
}

std::pair<bool, OrderState> OrderBook::getOrderStatus(int orderId) {
    if (activeOrderMap.find(orderId) != activeOrderMap.end()) {
        auto orderIt = activeOrderMap.at(orderId);
        int filledSize = orderIt->originalSize - orderIt->remainingSize;
        double averagePrice =  // construct adhoc for in-flight order
            static_cast<double>(orderIt->filledValue) / filledSize;
        return {true, OrderState{filledSize, averagePrice}};
    }
    if (doneOrderMap.find(orderId) != doneOrderMap.end()) {
        return {false, doneOrderMap.at(orderId)};
    }
    return {false, OrderState{}};  // orderId does not exist
}

std::pair<bool, OrderState> OrderBook::cancelOrder(int orderId) {
    auto [active, os] = getOrderStatus(orderId);
    if (!active) {
        return {false, os};  // order does not exist or is done
    }

    auto orderIt = activeOrderMap.at(orderId);
    int currIdx = orderIt->price / incr;
    orderLevels[currIdx].totalSize -= orderIt->remainingSize;
    orderLevels[currIdx].orders.erase(orderIt);
    activeOrderMap.erase(orderId);  // a cancelled order does not enter done map

    if (orderLevels[currIdx].totalSize == 0) {
        removeOrderLevel(currIdx);
    }

    return {true, os};
}

std::pair<bool, OrderState> OrderBook::updateOrder(int orderId, int newPrice,
                                                   int newSize) {
    auto [active, os] = getOrderStatus(orderId);
    if (!active) {
        return {false, os};  // order does not exist or is done
    }

    // Also error out if params are invalid or newSize < what has been done
    if ((!getIsOrderValid(newPrice, newSize)) || (os.filledSize >= newSize)) {
        return {false, os};
    }

    auto orderIt = activeOrderMap.at(orderId);
    int oldPrice = orderIt->price;
    if (oldPrice == newPrice) {
        orderIt->remainingSize = newSize - os.filledSize;
        orderIt->originalSize = newSize;
        return {true, os};
    }

    int currIdx = orderIt->price / incr;
    int price = newPrice;
    int orderSize = newSize - os.filledSize;
    bool isBid = getIsBid(currIdx);  // check side before cancelling

    cancelOrder(orderId);
    auto [_, newOrderId] = addOrder(price, orderSize, isBid);

    // We remap orderId to newOrderId because we don't return newOrderId
    activeOrderMap.insert_or_assign(orderId, std::move(activeOrderMap.at(newOrderId)));
    activeOrderMap.erase(newOrderId);

    return {true, os};
}

L1_Data OrderBook::getL1OrderData() {
    PriceLevel bestBid, bestOffer;
    if (lastBidIdx >= 0) {
        bestBid.price = lastBidIdx * incr;
        bestBid.totalSize = orderLevels[lastBidIdx].totalSize;
    }
    if (lastOfferIdx >= 0) {
        bestOffer.price = lastOfferIdx * incr;
        bestOffer.totalSize = orderLevels[lastOfferIdx].totalSize;
    }
    return {bestBid, bestOffer};
}

L2_Data OrderBook::getL2OrderData() {
    std::vector<PriceLevel> bids, offers;
    int currBidIdx = lastBidIdx;
    while (currBidIdx >= 0) {
        bids.push_back({currBidIdx * incr, orderLevels[currBidIdx].totalSize});
        currBidIdx = orderLevels[currBidIdx].prevIdx;
    }
    int currOfferIdx = lastOfferIdx;
    while (currOfferIdx >= 0) {
        offers.push_back(
            {currOfferIdx * incr, orderLevels[currOfferIdx].totalSize});
        currOfferIdx = orderLevels[currOfferIdx].prevIdx;
    }
    return {bids, offers};
}

/* Private members*/

inline std::pair<int, int> OrderBook::fillOrdersAtCurrIdx(const int currIdx,
                                                          int orderSize) {
    auto currOrderIt = orderLevels[currIdx].orders.begin();
    while (currOrderIt != orderLevels[currIdx].orders.end() && orderSize > 0) {
        int qtyFilled = std::min(orderSize, currOrderIt->remainingSize);
        orderSize -= qtyFilled;
        currOrderIt->remainingSize -= qtyFilled;
        currOrderIt->filledValue += qtyFilled * currIdx * incr;
        orderLevels[currIdx].totalSize -= qtyFilled;
        if (currOrderIt->remainingSize == 0) {
            // move from active orders to done orders (see .h for alternative)
            activeOrderMap.erase(currIdx);
            doneOrderMap.try_emplace(currIdx, currOrderIt->originalSize,
                           static_cast<double>(currOrderIt->filledValue) /
                               currOrderIt->originalSize);
            ++currOrderIt;
        }  // else orderSize = 0, will break in next iteration
    }

    // Remove from the linked list all orders that have been filled
    orderLevels[currIdx].orders.erase(orderLevels[currIdx].orders.begin(),
                                      currOrderIt);
    if (orderLevels[currIdx].totalSize == 0) {
        // totalSize = 0 for this level so go to next level
        return {orderLevels[currIdx].prevIdx, orderSize};
    }
    return {currIdx, orderSize};
}

void OrderBook::addNewOrderLevel(int newIdx, bool isBid) {
    if (isBid) {
        if (lastBidIdx < 0) {
            // this is the only bid
            lastBidIdx = newIdx;
            firstBidIdx = newIdx;
        } else if (lastBidIdx < newIdx) {
            // newBid is the highest bid
            orderLevels[newIdx].prevIdx = lastBidIdx;
            orderLevels[lastBidIdx].nextIdx = newIdx;
            lastBidIdx = newIdx;
        } else if (firstBidIdx > newIdx) {
            // newBid is the lowest bid
            orderLevels[newIdx].nextIdx = firstBidIdx;
            orderLevels[firstBidIdx].prevIdx = newIdx;
            firstBidIdx = newIdx;
        } else {
            // newBid is in the middle
            int currBidIdx = lastBidIdx;
            while (currBidIdx > newIdx) {
                currBidIdx = orderLevels[currBidIdx].prevIdx;
            }
            int nextBidIdx = orderLevels[currBidIdx].nextIdx;
            orderLevels[newIdx].nextIdx = nextBidIdx;
            orderLevels[newIdx].prevIdx = currBidIdx;
            orderLevels[currBidIdx].nextIdx = newIdx;
            orderLevels[nextBidIdx].prevIdx = newIdx;
        }
    } else {
        if (lastOfferIdx < 0) {
            // this is the only offer
            lastOfferIdx = newIdx;
            firstOfferIdx = newIdx;
        } else if (lastOfferIdx > newIdx) {
            // newOffer is the lowest offer
            orderLevels[newIdx].prevIdx = lastOfferIdx;
            orderLevels[lastOfferIdx].nextIdx = newIdx;
            lastOfferIdx = newIdx;
        } else if (firstOfferIdx < newIdx) {
            // newOffer is the highest offer
            orderLevels[newIdx].nextIdx = firstOfferIdx;
            orderLevels[firstOfferIdx].prevIdx = newIdx;
            firstOfferIdx = newIdx;
        } else {
            // newOffer is in the middle
            int currOfferIdx = lastOfferIdx;
            while (currOfferIdx < newIdx) {
                currOfferIdx = orderLevels[currOfferIdx].prevIdx;
            }
            int nextOfferIdx = orderLevels[currOfferIdx].nextIdx;
            orderLevels[newIdx].nextIdx = nextOfferIdx;
            orderLevels[newIdx].prevIdx = currOfferIdx;
            orderLevels[currOfferIdx].nextIdx = newIdx;
            orderLevels[nextOfferIdx].prevIdx = newIdx;
        }
    }
}

void OrderBook::removeOrderLevel(int currIdx) {
    bool isBid = getIsBid(currIdx);

    // We can skip updating our own pointers because they are now unreachable
    int nextIdx = orderLevels[currIdx].nextIdx;
    int prevIdx = orderLevels[currIdx].prevIdx;

    // However we need to update adjacent OrderLevels
    if (nextIdx >= 0) {
        orderLevels[nextIdx].prevIdx = prevIdx;
    }
    if (prevIdx >= 0) {
        orderLevels[prevIdx].nextIdx = nextIdx;
    }

    // Update global max and min
    if (isBid) {
        if (nextIdx < 0) {
            lastBidIdx = prevIdx;
        }
        if (prevIdx < 0) {
            firstBidIdx = nextIdx;
        }
    } else {
        if (nextIdx < 0) {
            lastOfferIdx = prevIdx;
        }
        if (prevIdx < 0) {
            firstBidIdx = nextIdx;
        }
    }
}

