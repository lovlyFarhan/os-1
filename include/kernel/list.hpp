#ifndef __LIST_HPP__
#define __LIST_HPP__

#include <assert.h>
#include <stdint.h>

class ListElement
{
public:
    ListElement()
        : prev(this)
        , next(this)
    {
    }

    ListElement * prev;
    ListElement * next;
};

template <class T, ListElement T::* Ptr>
    class List
    {
    public:

        /**
         * Delete-safe iterator. Suggested use:
         *
         * List<Foo> list;
         * ...
         *
         * for (List<Foo>::Iterator i = list.Begin(); i; i++) {
         *     Foo * element = *i;
         *     ...;
         *     if (someThing) {
         *         list.Remove(element);
         *     }
         * }
         */
        class Iterator
        {
        friend class List;

        private:
            Iterator (List<T, Ptr> * list, ListElement * elem)
                : mList(list)
                , mElem(elem)
            {
                mNextElem = mElem->next;
            }

            void Advance ()
            {
                mElem = mNextElem;
                mNextElem = mElem->next;
            }

        private:
            List<T, Ptr> *  mList;
            ListElement *   mElem;
            ListElement *   mNextElem;

        public:
            T * operator -> ()
            {
                return mList->elemFromHead(mElem);
            }

            T * operator * ()
            {
                return mList->elemFromHead(mElem);
            }

            operator bool ()
            {
                return mElem != &mList->mHead;
            }

            Iterator operator ++ (int dummy)
            {
                Iterator prevalue = *this;
                Advance();
                return prevalue;
            }

            Iterator operator ++ ()
            {
                Advance();
                return *this;
            }
        };

        List ()
        {
        }

        ~List ()
        {
            assert(Empty());
        }

        Iterator Begin () {
            return Iterator(this, mHead.next);
        }

        bool Empty () {
            return &mHead == mHead.next;
        }

        void Prepend (T * element) {
            ListElement * elementHead = &(element->*Ptr);
            elementHead->prev = &mHead;
            elementHead->next = mHead.next;
            mHead.next->prev = elementHead;
            mHead.next = elementHead;
        }

        void Append (T * element) {
            ListElement * elementHead = &(element->*Ptr);
            elementHead->prev = mHead.prev;
            elementHead->next = &mHead;
            mHead.prev->next = elementHead;
            mHead.prev = elementHead;
        }

        void Remove (T * element) {
            assert(!Empty());

            ListElement * elementHead = &(element->*Ptr);
            elementHead->prev->next = elementHead->next;
            elementHead->next->prev = elementHead->prev;
            elementHead->next = elementHead->prev = elementHead;
        }

        T * First () {
            return elemFromHead(mHead.next);
        }

        T * Next (T * element) {
            ListElement * elementHead = &(element->*Ptr);
            return elemFromHead(elementHead->next);
        }

        T * Prev (T * element) {
            ListElement * elementHead = &(element->*Ptr);
            return elemFromHead(elementHead->prev);
        }

    private:
        ListElement         mHead;

        T * elemFromHead (ListElement * head) {

            ListElement * zeroHead = &(static_cast<T*>(0)->*Ptr);
            uintptr_t offset = reinterpret_cast<uintptr_t>(zeroHead);

            T * result = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(head) - offset);
            return result;
        }
    };

#endif /* __LIST_HPP__ */