#ifndef CURLMULTIASIO_MULTI_H_
#define CURLMULTIASIO_MULTI_H_

/// @file
/// cURL multi handle
/// 6/21/21 11:45

// curl-multi-asio includes
#include <curl-multi-asio/Common.h>
#include <curl-multi-asio/Detail/Lifetime.h>
#include <curl-multi-asio/Easy.h>
#include <curl-multi-asio/Error.h>

// STL includes
#include <atomic>
#include <unordered_map>
#include <utility>

template<typename T>
concept HasExecutor = requires(T a)
{
	{ a.get_executor() };
};

namespace cma
{
	/// @brief Multi is a multi handle, which tracks and executes
	/// all curl_multi calls
	class Multi
	{
	private:
		/// @brief These handlers store all of the handler data including
		/// the raw socket, and the handler itself. They also handle
		/// unregistration
		class PerformHandlerBase
		{
		public:
			PerformHandlerBase(CURL* easyHandle, CURLM* multiHandle) noexcept :
				m_easyHandle(easyHandle), m_multiHandle(multiHandle) {}
			virtual ~PerformHandlerBase() = default;

			/// @brief Completes the perform, and calls the handler. Must
			/// set handled status
			/// @param ec The error code
			/// @param e The curl error
			virtual void Complete(error_code ec) noexcept = 0;

			/// @return The underlying easy handle
			inline CURL* GetEasyHandle() const noexcept { return m_easyHandle; }
			/// @return The underlying multi handle
			inline CURLM* GetMultiHandle() const noexcept { return m_multiHandle; }
			/// @return If the handler was considered handled
			inline bool Handled() const noexcept { return m_handled; }
		protected:
			/// @param handled If the handle was considered handled
			inline void SetHandled(bool handled) noexcept { m_handled = handled; }
		private:
			CURL* m_easyHandle;
			CURL* m_multiHandle;
			bool m_handled = false;
		};
		template<typename Handler>
		class PerformHandler : public PerformHandlerBase
		{
		public:
			PerformHandler(CURL* easyHandle, CURLM* multiHandle, Handler& handler) noexcept :
				PerformHandlerBase(easyHandle, multiHandle), m_handler(std::move(handler)) {}
			~PerformHandler() noexcept
			{
				// abort if we haven't been handled
				if (Handled() == false)
					Complete(asio::error::operation_aborted);
			}

			void Complete(cma::error_code ec) noexcept
			{
				if (Handled() == true)
					return;
				// remove the handler from the multi handle
				curl_multi_remove_handle(GetMultiHandle(), GetEasyHandle());
				m_handler(ec);
				SetHandled(true);
			}
		private:
			Handler m_handler;
		};
	public:
		/// @brief Creates the handle and if necessary, initializes cURL.
		/// If CMA_MANAGE_CURL is specified when the library is built,
		/// cURL's lifetime is managed by the total instances of Multi,
		/// and curl_global_init will be called by the library.
		/// If you would rather manage the lifetime yourself, an interface
		/// is provided in cma::Detail::Lifetime.
		/// @param executor The executor type
		Multi(const asio::any_io_executor& executor) noexcept;
		/// @brief Creates the handle and if necessary, initializes cURL.
		/// If CMA_MANAGE_CURL is specified when the library is built,
		/// cURL's lifetime is managed by the total instances of Multi,
		/// and curl_global_init will be called by the library.
		/// If you would rather manage the lifetime yourself, an interface
		/// is provided in cma::Detail::Lifetime.
		/// @tparam ExecutionContext The execution context type
		/// @param ctx The execution context
		template<HasExecutor ExecutionContext>
		explicit Multi(ExecutionContext& ctx)
			: Multi(ctx.get_executor()) {}
		/// @brief Cancels any outstanding operations, and destroys handles.
		/// If CMA_MANAGE_CURL is specified when the library is built and
		/// this is the only instance of Multi, curl_global_cleanup will be called
		~Multi() noexcept { cma::error_code ignored; Cancel(ignored); }
		// we don't allow copies, because multi handles can't be duplicated.
		// there's not even a reason to do so, multi handles don't really hold
		// much of a state themselves besides stuff that shouldn't be duplicated
		Multi(const Multi&) = delete;
		Multi& operator=(const Multi&) = delete;
		/// @brief The other multi instance ends up in an invalid state
		Multi(Multi&& other) = default;
		/// @brief The other multi instance ends up in an invalid state
		/// @return This multi handle
		Multi& operator=(Multi&& other) = default;

		/// @return The associated executor
		inline asio::any_io_executor& GetExecutor() noexcept { return m_executor; }
		/// @return The native handle
		inline CURLM* GetNativeHandle() const noexcept { return m_nativeHandle.get(); }

		/// @return Whether or not the handle is valid
		inline operator bool() const noexcept { return m_nativeHandle != nullptr; }

		/// @brief Launches an asynchronous perform operation, and notifies
		/// the completion token either on error or success. This can be called
		/// from multiple threads at once. Once the operation is initiated,
		/// it is the responsibility of the caller to ensure that the easy handle
		/// stays in scope until the handler is called. The completon token
		/// signature is void(error_code)
		/// @tparam CompletionToken The completion token type
		/// @param easyHandle The easy handle to perform the action on
		/// @param token The completion token
		/// @return DEDUCED
		template<typename CompletionToken>
		auto AsyncPerform(Easy& easyHandle, CompletionToken&& token)
		{
			auto initiation = [this](auto&& handler, Easy& easy)
			{
				// do this in a strand so that curl can't be accessed concurrently
				asio::post(m_executor, asio::bind_executor(m_strand,
					[this, handler = std::move(handler), &easy]() mutable
				{
					// set the open and close socket functions. this allows
					// us to make them asio sockets for async functionality
					easy.SetOption(CURLoption::CURLOPT_OPENSOCKETFUNCTION, &Multi::OpenSocketCb);
					easy.SetOption(CURLoption::CURLOPT_OPENSOCKETDATA, this);
					easy.SetOption(CURLoption::CURLOPT_CLOSESOCKETFUNCTION, &Multi::CloseSocketCb);
					easy.SetOption(CURLoption::CURLOPT_CLOSESOCKETDATA, this);
					// store the handler
					auto performHandler = std::make_unique<PerformHandler<
						typename std::decay_t<decltype(handler)>>>(
							easy.GetNativeHandle(), GetNativeHandle(), handler);
					// track the socket and initiate the transfer. if this fails
					if (auto res = curl_multi_add_handle(GetNativeHandle(),
						easy.GetNativeHandle()); res != CURLM_OK)
						return performHandler->Complete(res);
					// track the handler
					m_easyHandlerMap.emplace(easy.GetNativeHandle(), std::move(performHandler));
				}));
			};
			return asio::async_initiate<CompletionToken,
				void(error_code)>(initiation, token, std::ref(easyHandle));
		}
		/// @brief Cancels all outstanding asynchronous operations,
		/// and calls handlers with asio::error::operation_aborted.
		/// The easy handles must stay in scope until their handlers
		/// have been called.
		/// @param ec The error code output
		/// @param error The error to send to all open handlers
		/// @return The number of asynchronous operations canceled
		size_t Cancel(cma::error_code& ec,
			CURLMcode error = CURLMcode::CURLM_OK) noexcept;
		/// @brief Cancels the outstanding asynchronous operation,
		/// and calls the handler with asio::error::operation_aborted.
		/// The easy handle must stay in scope until its handler has
		/// been called
		/// @param easy The easy handle
		/// @param error Te error to send to all open handlers
		/// @return Whether or not the handler was canceled
		bool Cancel(const Easy& easy, CURLMcode error = CURLMcode::CURLM_OK) noexcept;

		/// @brief Sets a multi option
		/// @tparam T The option value type
		/// @param option The option
		/// @param val The value
		/// @return The resulting error
		template<typename T>
		inline error_code SetOption(CURLMoption option, T&& val) noexcept
		{
			// weird GCC bug where forward thinks its return value is ignored
			return curl_multi_setopt(GetNativeHandle(), option, static_cast<T&&>(val));
		}
	private:
		/// @brief Closes a socket, and then we can free the socket. For a
		/// description of arguments, check cURL documentation for
		/// CURLOPT_CLOSESOCKETFUNCTION
		/// @return 0 on success, CURL_BADSOCKET on failure
		static int CloseSocketCb(Multi* clientp, curl_socket_t item) noexcept;
		/// @brief Opens an asio socket for an address. For a description
		/// of arguments, check cURL documentation for CURLOPT_OPENSOCKETFUNCTION
		/// @return The socket
		static curl_socket_t OpenSocketCb(Multi* clientp, curlsocktype purpose,
			curl_sockaddr* address) noexcept;
		/// @brief The socket callback called by cURL when a socket should
		/// read, write, or be destroyed. For a description of arguments,
		/// check cURL documentation for CURLMOPT_SOCKETFUNCTION
		/// @return 0 on success
		static int SocketCallback(CURL* easy, curl_socket_t s, int what,
			Multi* userp, int* socketp) noexcept;
		/// @brief The timer callback called by cURL when a timer should be set.
		/// For a description on arguments, check cURL documentation for
		/// CURLMOPT_TIMERFUNCTION
		/// @return 0 on success, 1 on failure
		static int TimerCallback(CURLM* multi, long timeout_ms, Multi* userp) noexcept;

		/// @brief Checks the handle for completed handles and calls any
		/// completion handlers for finished transfers, before removing them
		void CheckTransfers() noexcept;
		/// @brief Handles socket events for reads and writes
		/// @param ec The error code
		/// @param s The socket
		/// @param what The type of event
		void EventCallback(const cma::error_code& ec, curl_socket_t s,
			int what, int* last) noexcept;
		asio::any_io_executor m_executor;
#ifdef CMA_MANAGE_CURL
		Detail::Lifetime s_lifetime;
#endif
		// when the handlers are destructed, their curl handle must be untracked
		std::unordered_map<CURL*, std::unique_ptr<PerformHandlerBase>> m_easyHandlerMap;
		std::unordered_map<curl_socket_t, asio::ip::tcp::socket> m_easySocketMap;
		asio::system_timer m_timer;
		asio::strand<asio::any_io_executor> m_strand;
		std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> m_nativeHandle;
	};
}

#endif