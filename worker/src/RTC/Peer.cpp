#define MS_CLASS "RTC::Peer"
// #define MS_LOG_DEV

#include "RTC/Peer.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupError.hpp"
#include "RTC/RTCP/CompoundPacket.hpp"
#include "RTC/RTCP/FeedbackPsRemb.hpp"
#include "RTC/RTCP/FeedbackRtpNack.hpp"
#include "RTC/RTCP/Sdes.hpp"
#include "RTC/RtpDictionaries.hpp"

namespace RTC
{
	/* Instance methods. */

	Peer::Peer(Listener* listener, Channel::Notifier* notifier, uint32_t peerId, std::string& peerName)
	    : peerId(peerId), peerName(peerName), listener(listener), notifier(notifier)
	{
		MS_TRACE();

		this->timer = new Timer(this);

		// Start the RTCP timer.
		this->timer->Start(static_cast<uint64_t>(RTC::RTCP::MaxVideoIntervalMs / 2));
	}

	Peer::~Peer()
	{
		MS_TRACE();

		// Destroy the RTCP timer.
		this->timer->Destroy();
	}

	void Peer::Destroy()
	{
		MS_TRACE();

		static const Json::StaticString JsonStringClass{ "class" };

		Json::Value eventData(Json::objectValue);

		// Close all the Producers.
		for (auto it = this->producers.begin(); it != this->producers.end();)
		{
			auto* producer = it->second;

			it = this->producers.erase(it);
			producer->Destroy();
		}

		// Close all the Consumers.
		for (auto it = this->consumers.begin(); it != this->consumers.end();)
		{
			auto* consumer = it->second;

			it = this->consumers.erase(it);
			consumer->Destroy();
		}

		// Close all the Transports.
		// NOTE: It is critical to close Transports after Producers/Consumers
		// because RtcReceiver.Destroy() fires an event in the Transport.
		for (auto it = this->transports.begin(); it != this->transports.end();)
		{
			auto* transport = it->second;

			it = this->transports.erase(it);
			transport->Destroy();
		}

		// Notify.
		eventData[JsonStringClass] = "Peer";
		this->notifier->Emit(this->peerId, "close", eventData);

		// Notify the listener.
		this->listener->OnPeerClosed(this);

		delete this;
	}

	Json::Value Peer::ToJson() const
	{
		MS_TRACE();

		static const Json::StaticString JsonStringPeerId{ "peerId" };
		static const Json::StaticString JsonStringPeerName{ "peerName" };
		static const Json::StaticString JsonStringCapabilities{ "capabilities" };
		static const Json::StaticString JsonStringTransports{ "transports" };
		static const Json::StaticString JsonStringProducers{ "producers" };
		static const Json::StaticString JsonStringConsumers{ "consumers" };

		Json::Value json(Json::objectValue);
		Json::Value jsonTransports(Json::arrayValue);
		Json::Value jsonProducers(Json::arrayValue);
		Json::Value jsonConsumers(Json::arrayValue);

		// Add `peerId`.
		json[JsonStringPeerId] = Json::UInt{ this->peerId };

		// Add `peerName`.
		json[JsonStringPeerName] = this->peerName;

		// Add `capabilities`.
		if (this->hasCapabilities)
			json[JsonStringCapabilities] = this->capabilities.ToJson();

		// Add `transports`.
		for (auto& kv : this->transports)
		{
			auto* transport = kv.second;

			jsonTransports.append(transport->ToJson());
		}
		json[JsonStringTransports] = jsonTransports;

		// Add `producers`.
		for (auto& kv : this->producers)
		{
			auto* producer = kv.second;

			jsonProducers.append(producer->ToJson());
		}
		json[JsonStringProducers] = jsonProducers;

		// Add `consumers`.
		for (auto& kv : this->consumers)
		{
			auto* consumer = kv.second;

			jsonConsumers.append(consumer->ToJson());
		}
		json[JsonStringConsumers] = jsonConsumers;

		return json;
	}

	void Peer::HandleRequest(Channel::Request* request)
	{
		MS_TRACE();

		switch (request->methodId)
		{
			case Channel::Request::MethodId::PEER_CLOSE:
			{
#ifdef MS_LOG_DEV
				uint32_t peerId = this->peerId;
#endif

				Destroy();

				MS_DEBUG_DEV("Peer closed [peerId:%" PRIu32 "]", peerId);

				request->Accept();

				break;
			}

			case Channel::Request::MethodId::PEER_DUMP:
			{
				auto json = ToJson();

				request->Accept(json);

				break;
			}

			case Channel::Request::MethodId::PEER_SET_CAPABILITIES:
			{
				// Capabilities must not be set.
				if (this->hasCapabilities)
				{
					request->Reject("peer capabilities already set");

					return;
				}

				try
				{
					this->capabilities = RTC::RtpCapabilities(request->data);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				this->hasCapabilities = true;

				// Notify the listener (Room) who will remove capabilities to make them
				// a subset of the room capabilities.
				this->listener->OnPeerCapabilities(this, std::addressof(this->capabilities));

				Json::Value data = this->capabilities.ToJson();

				// NOTE: We accept the request *after* calling onPeerCapabilities(). This
				// guarantees that the Peer will receive a "newconsumer" event for all its
				// associated Consumers *before* the setCapabilities() Promise resolves.
				// In other words, at the time setCapabilities() resolves, the Peer already
				// has set all its current Consumers.
				request->Accept(data);

				break;
			}

			case Channel::Request::MethodId::PEER_CREATE_TRANSPORT:
			{
				RTC::Transport* transport;
				uint32_t transportId;

				try
				{
					transport = GetTransportFromRequest(request, &transportId);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (transport != nullptr)
				{
					request->Reject("Transport already exists");

					return;
				}

				try
				{
					transport = new RTC::Transport(this, this->notifier, transportId, request->data);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				this->transports[transportId] = transport;

				MS_DEBUG_DEV("Transport created [transportId:%" PRIu32 "]", transportId);

				auto data = transport->ToJson();

				request->Accept(data);

				break;
			}

			case Channel::Request::MethodId::PEER_CREATE_PRODUCER:
			{
				static const Json::StaticString JsonStringKind{ "kind" };

				RTC::Producer* producer;
				RTC::Transport* transport{ nullptr };
				uint32_t producerId;

				// Capabilities must be set.
				if (!this->hasCapabilities)
				{
					request->Reject("peer capabilities are not yet set");

					return;
				}

				try
				{
					producer = GetProducerFromRequest(request, &producerId);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (producer != nullptr)
				{
					request->Reject("Producer already exists");

					return;
				}

				try
				{
					transport = GetTransportFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (transport == nullptr)
				{
					request->Reject("Transport does not exist");

					return;
				}

				// `kind` is mandatory.

				if (!request->data[JsonStringKind].isString())
					MS_THROW_ERROR("missing kind");

				std::string kind = request->data[JsonStringKind].asString();

				// Create a Producer instance.
				try
				{
					producer = new RTC::Producer(this, this->notifier, producerId, RTC::Media::GetKind(kind));
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				this->producers[producerId] = producer;

				MS_DEBUG_DEV("Producer created [producerId:%" PRIu32 "]", producerId);

				// Set the Transport.
				producer->SetTransport(transport);

				request->Accept();

				break;
			}

			case Channel::Request::MethodId::TRANSPORT_CLOSE:
			case Channel::Request::MethodId::TRANSPORT_DUMP:
			case Channel::Request::MethodId::TRANSPORT_SET_REMOTE_DTLS_PARAMETERS:
			case Channel::Request::MethodId::TRANSPORT_SET_MAX_BITRATE:
			case Channel::Request::MethodId::TRANSPORT_CHANGE_UFRAG_PWD:
			{
				RTC::Transport* transport;

				try
				{
					transport = GetTransportFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (transport == nullptr)
				{
					request->Reject("Transport does not exist");

					return;
				}

				transport->HandleRequest(request);

				break;
			}

			case Channel::Request::MethodId::PRODUCER_CLOSE:
			case Channel::Request::MethodId::PRODUCER_DUMP:
			case Channel::Request::MethodId::PRODUCER_RECEIVE:
			case Channel::Request::MethodId::PRODUCER_SET_RTP_RAW_EVENT:
			case Channel::Request::MethodId::PRODUCER_SET_RTP_OBJECT_EVENT:
			{
				RTC::Producer* producer;

				try
				{
					producer = GetProducerFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (producer == nullptr)
				{
					request->Reject("Producer does not exist");

					return;
				}

				producer->HandleRequest(request);

				break;
			}

			case Channel::Request::MethodId::PRODUCER_SET_TRANSPORT:
			{
				RTC::Producer* producer;

				try
				{
					producer = GetProducerFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (producer == nullptr)
				{
					request->Reject("Producer does not exist");

					return;
				}

				RTC::Transport* transport;

				try
				{
					transport = GetTransportFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (transport == nullptr)
				{
					request->Reject("Transport does not exist");

					return;
				}

				try
				{
					// NOTE: This may throw.
					transport->AddProducer(producer);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				// Enable REMB in the new transport if it was enabled in the previous one.
				auto previousTransport = producer->GetTransport();

				if ((previousTransport != nullptr) && previousTransport->HasRemb())
					transport->EnableRemb();

				producer->SetTransport(transport);

				request->Accept();

				break;
			}

			case Channel::Request::MethodId::CONSUMER_DUMP:
			{
				RTC::Consumer* consumer;

				try
				{
					consumer = GetConsumerFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (consumer == nullptr)
				{
					request->Reject("Consumer does not exist");

					return;
				}

				consumer->HandleRequest(request);

				break;
			}

			case Channel::Request::MethodId::CONSUMER_SET_TRANSPORT:
			{
				RTC::Consumer* consumer;

				try
				{
					consumer = GetConsumerFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (consumer == nullptr)
				{
					request->Reject("Consumer does not exist");

					return;
				}

				RTC::Transport* transport;

				try
				{
					transport = GetTransportFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (transport == nullptr)
				{
					request->Reject("Transport does not exist");

					return;
				}

				consumer->SetTransport(transport);

				request->Accept();

				break;
			}

			case Channel::Request::MethodId::CONSUMER_DISABLE:
			{
				RTC::Consumer* consumer;

				try
				{
					consumer = GetConsumerFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				if (consumer == nullptr)
				{
					request->Reject("Consumer does not exist");

					return;
				}

				consumer->HandleRequest(request);

				break;
			}

			default:
			{
				MS_ERROR("unknown method");

				request->Reject("unknown method");
			}
		}
	}

	void Peer::AddConsumer(
	    RTC::Consumer* consumer, RTC::RtpParameters* rtpParameters, uint32_t associatedProducerId)
	{
		MS_TRACE();

		static const Json::StaticString JsonStringClass{ "class" };
		static const Json::StaticString JsonStringConsumerId{ "consumerId" };
		static const Json::StaticString JsonStringKind{ "kind" };
		static const Json::StaticString JsonStringRtpParameters{ "rtpParameters" };
		static const Json::StaticString JsonStringActive{ "active" };
		static const Json::StaticString JsonStringAssociatedProducerId{ "associatedProducerId" };

		MS_ASSERT(
		    this->consumers.find(consumer->consumerId) == this->consumers.end(),
		    "given Consumer already exists in this Peer");

		// Provide the Consumer with peer's capabilities.
		consumer->SetPeerCapabilities(std::addressof(this->capabilities));

		// Provide the Consumer with the received RTP parameters.
		consumer->Send(rtpParameters);

		// Store it.
		this->consumers[consumer->consumerId] = consumer;

		// Notify.
		Json::Value eventData = consumer->ToJson();

		eventData[JsonStringClass]                = "Peer";
		eventData[JsonStringConsumerId]           = Json::UInt{ consumer->consumerId };
		eventData[JsonStringKind]                 = RTC::Media::GetJsonString(consumer->kind);
		eventData[JsonStringRtpParameters]        = consumer->GetParameters()->ToJson();
		eventData[JsonStringActive]               = consumer->GetActive();
		eventData[JsonStringAssociatedProducerId] = Json::UInt{ associatedProducerId };

		this->notifier->Emit(this->peerId, "newconsumer", eventData);
	}

	RTC::Consumer* Peer::GetConsumer(uint32_t ssrc) const
	{
		MS_TRACE();

		auto it = this->consumers.begin();
		for (; it != this->consumers.end(); ++it)
		{
			auto consumer      = it->second;
			auto rtpParameters = consumer->GetParameters();

			if (rtpParameters == nullptr)
				continue;

			auto it2 = rtpParameters->encodings.begin();
			for (; it2 != rtpParameters->encodings.end(); ++it2)
			{
				auto& encoding = *it2;

				if (encoding.ssrc == ssrc)
					return consumer;
				if (encoding.hasFec && encoding.fec.ssrc == ssrc)
					return consumer;
				if (encoding.hasRtx && encoding.rtx.ssrc == ssrc)
					return consumer;
			}
		}

		return nullptr;
	}

	void Peer::SendRtcp(uint64_t now)
	{
		MS_TRACE();

		// For every transport:
		// - Create a CompoundPacket.
		// - Request every Sender and Receiver of such transport their RTCP data.
		// - Send the CompoundPacket.

		for (auto& it : this->transports)
		{
			std::unique_ptr<RTC::RTCP::CompoundPacket> packet(new RTC::RTCP::CompoundPacket());
			auto* transport = it.second;

			for (auto& it : this->consumers)
			{
				auto* consumer = it.second;

				if (consumer->GetTransport() != transport)
					continue;

				consumer->GetRtcp(packet.get(), now);

				// Send one RTCP compound packet per sender report.
				if (packet->GetSenderReportCount() != 0u)
				{
					// Ensure that the RTCP packet fits into the RTCP buffer.
					if (packet->GetSize() > RTC::RTCP::BufferSize)
					{
						MS_WARN_TAG(rtcp, "cannot send RTCP packet, size too big (%zu bytes)", packet->GetSize());

						return;
					}

					packet->Serialize(RTC::RTCP::Buffer);
					transport->SendRtcpCompoundPacket(packet.get());
					// Reset the Compound packet.
					packet.reset(new RTC::RTCP::CompoundPacket());
				}
			}

			for (auto& it : this->producers)
			{
				auto* producer = it.second;

				if (producer->GetTransport() != transport)
					continue;

				producer->GetRtcp(packet.get(), now);
			}

			// Send one RTCP compound with all receiver reports.
			if (packet->GetReceiverReportCount() != 0u)
			{
				// Ensure that the RTCP packet fits into the RTCP buffer.
				if (packet->GetSize() > RTC::RTCP::BufferSize)
				{
					MS_WARN_TAG(rtcp, "cannot send RTCP packet, size too big (%zu bytes)", packet->GetSize());

					return;
				}

				packet->Serialize(RTC::RTCP::Buffer);
				transport->SendRtcpCompoundPacket(packet.get());
			}
		}
	}

	RTC::Transport* Peer::GetTransportFromRequest(Channel::Request* request, uint32_t* transportId) const
	{
		MS_TRACE();

		static const Json::StaticString JsonStringTransportId{ "transportId" };

		auto jsonTransportId = request->internal[JsonStringTransportId];

		if (!jsonTransportId.isUInt())
			MS_THROW_ERROR("Request has not numeric internal.transportId");

		if (transportId != nullptr)
			*transportId = jsonTransportId.asUInt();

		auto it = this->transports.find(jsonTransportId.asUInt());
		if (it != this->transports.end())
		{
			auto* transport = it->second;

			return transport;
		}

		return nullptr;
	}

	RTC::Producer* Peer::GetProducerFromRequest(Channel::Request* request, uint32_t* producerId) const
	{
		MS_TRACE();

		static const Json::StaticString JsonStringProducerId{ "producerId" };

		auto jsonProducerId = request->internal[JsonStringProducerId];

		if (!jsonProducerId.isUInt())
			MS_THROW_ERROR("Request has not numeric internal.producerId");

		if (producerId != nullptr)
			*producerId = jsonProducerId.asUInt();

		auto it = this->producers.find(jsonProducerId.asUInt());
		if (it != this->producers.end())
		{
			auto* producer = it->second;

			return producer;
		}

		return nullptr;
	}

	RTC::Consumer* Peer::GetConsumerFromRequest(Channel::Request* request, uint32_t* consumerId) const
	{
		MS_TRACE();

		static const Json::StaticString JsonStringConsumerId{ "consumerId" };

		auto jsonConsumerId = request->internal[JsonStringConsumerId];

		if (!jsonConsumerId.isUInt())
			MS_THROW_ERROR("Request has not numeric internal.consumerId");

		if (consumerId != nullptr)
			*consumerId = jsonConsumerId.asUInt();

		auto it = this->consumers.find(jsonConsumerId.asUInt());
		if (it != this->consumers.end())
		{
			auto* consumer = it->second;

			return consumer;
		}

		return nullptr;
	}

	void Peer::OnTransportConnected(RTC::Transport* transport)
	{
		MS_TRACE();

		// If the transport is used by any Consumer (video/depth) notify the
		// listener.
		for (auto& kv : this->consumers)
		{
			auto* consumer = kv.second;

			if (consumer->kind != RTC::Media::Kind::VIDEO && consumer->kind != RTC::Media::Kind::DEPTH)
			{
				continue;
			}

			if (consumer->GetTransport() != transport)
				continue;

			this->listener->OnFullFrameRequired(this, consumer);
		}
	}

	void Peer::OnTransportClosed(RTC::Transport* transport)
	{
		MS_TRACE();

		// Must remove the closed Transport from all the Producers holding it.
		for (auto& kv : this->producers)
		{
			auto* producer = kv.second;

			producer->RemoveTransport(transport);
		}

		// Must also unset this Transport from all the Consumers using it.
		for (auto& kv : this->consumers)
		{
			auto* consumer = kv.second;

			consumer->RemoveTransport(transport);
		}

		this->transports.erase(transport->transportId);
	}

	void Peer::OnTransportFullFrameRequired(RTC::Transport* transport)
	{
		MS_TRACE();

		// If the transport is used by any Producer (video/depth) notify the
		// listener.
		for (auto& kv : this->producers)
		{
			auto* producer = kv.second;

			if (producer->kind != RTC::Media::Kind::VIDEO && producer->kind != RTC::Media::Kind::DEPTH)
			{
				continue;
			}

			if (producer->GetTransport() != transport)
				continue;

			producer->RequestFullFrame();
		}
	}

	void Peer::OnProducerParameters(RTC::Producer* producer)
	{
		MS_TRACE();

		auto rtpParameters = producer->GetParameters();

		// Remove unsupported codecs and their associated encodings.
		rtpParameters->ReduceCodecsAndEncodings(this->capabilities);

		// Remove unsupported header extensions.
		rtpParameters->ReduceHeaderExtensions(this->capabilities.headerExtensions);

		auto transport = producer->GetTransport();

		// NOTE: This may throw.
		if (transport != nullptr)
			transport->AddProducer(producer);
	}

	void Peer::OnProducerParametersDone(RTC::Producer* producer)
	{
		MS_TRACE();

		// Notify the listener (Room).
		this->listener->OnPeerProducerParameters(this, producer);
	}

	void Peer::OnRtpPacket(RTC::Producer* producer, RTC::RtpPacket* packet)
	{
		MS_TRACE();

		// Notify the listener.
		this->listener->OnPeerRtpPacket(this, producer, packet);
	}

	void Peer::OnTransportRtcpPacket(RTC::Transport* transport, RTC::RTCP::Packet* packet)
	{
		MS_TRACE();

		while (packet != nullptr)
		{
			switch (packet->GetType())
			{
				/* RTCP coming from a remote Producer which must be forwarded to the corresponding
				 * remote Consumer. */

				case RTCP::Type::RR:
				{
					auto* rr = dynamic_cast<RTCP::ReceiverReportPacket*>(packet);
					auto it  = rr->Begin();

					for (; it != rr->End(); ++it)
					{
						auto& report   = (*it);
						auto* consumer = this->GetConsumer(report->GetSsrc());

						if (consumer != nullptr)
						{
							this->listener->OnPeerRtcpReceiverReport(this, consumer, report);
						}
						else
						{
							MS_WARN_TAG(
							    rtcp,
							    "no Consumer found for received Receiver Report [ssrc:%" PRIu32 "]",
							    report->GetSsrc());
						}
					}

					break;
				}

				case RTCP::Type::PSFB:
				{
					auto* feedback = dynamic_cast<RTCP::FeedbackPsPacket*>(packet);

					switch (feedback->GetMessageType())
					{
						case RTCP::FeedbackPs::MessageType::AFB:
						{
							auto* afb = dynamic_cast<RTCP::FeedbackPsAfbPacket*>(feedback);

							if (afb->GetApplication() == RTCP::FeedbackPsAfbPacket::Application::REMB)
								break;
						}

						// [[fallthrough]]; (C++17)
						case RTCP::FeedbackPs::MessageType::PLI:
						case RTCP::FeedbackPs::MessageType::SLI:
						case RTCP::FeedbackPs::MessageType::RPSI:
						case RTCP::FeedbackPs::MessageType::FIR:
						{
							auto* consumer = this->GetConsumer(feedback->GetMediaSsrc());

							if (consumer != nullptr)
							{
								// If the Consumer is not active, drop the packet.
								if (!consumer->GetActive())
									break;

								if (feedback->GetMessageType() == RTCP::FeedbackPs::MessageType::PLI)
								{
									MS_DEBUG_TAG(rtx, "PLI received [media ssrc:%" PRIu32 "]", feedback->GetMediaSsrc());
								}

								this->listener->OnPeerRtcpFeedback(this, consumer, feedback);
							}
							else
							{
								MS_WARN_TAG(
								    rtcp,
								    "no Consumer found for received %s Feedback packet "
								    "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
								    RTCP::FeedbackPsPacket::MessageType2String(feedback->GetMessageType()).c_str(),
								    feedback->GetMediaSsrc(),
								    feedback->GetMediaSsrc());
							}

							break;
						}

						case RTCP::FeedbackPs::MessageType::TSTR:
						case RTCP::FeedbackPs::MessageType::TSTN:
						case RTCP::FeedbackPs::MessageType::VBCM:
						case RTCP::FeedbackPs::MessageType::PSLEI:
						case RTCP::FeedbackPs::MessageType::ROI:
						case RTCP::FeedbackPs::MessageType::EXT:
						default:
						{
							MS_WARN_TAG(
							    rtcp,
							    "ignoring unsupported %s Feedback packet "
							    "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
							    RTCP::FeedbackPsPacket::MessageType2String(feedback->GetMessageType()).c_str(),
							    feedback->GetMediaSsrc(),
							    feedback->GetMediaSsrc());

							break;
						}
					}

					break;
				}

				case RTCP::Type::RTPFB:
				{
					auto* feedback = dynamic_cast<RTCP::FeedbackRtpPacket*>(packet);

					switch (feedback->GetMessageType())
					{
						case RTCP::FeedbackRtp::MessageType::NACK:
						{
							auto* consumer = this->GetConsumer(feedback->GetMediaSsrc());

							if (consumer != nullptr)
							{
								auto* nackPacket = dynamic_cast<RTC::RTCP::FeedbackRtpNackPacket*>(packet);

								consumer->ReceiveNack(nackPacket);
							}
							else
							{
								MS_WARN_TAG(
								    rtcp,
								    "no Consumer found for received NACK Feedback packet "
								    "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
								    feedback->GetMediaSsrc(),
								    feedback->GetMediaSsrc());
							}

							break;
						}

						case RTCP::FeedbackRtp::MessageType::TMMBR:
						case RTCP::FeedbackRtp::MessageType::TMMBN:
						case RTCP::FeedbackRtp::MessageType::SR_REQ:
						case RTCP::FeedbackRtp::MessageType::RAMS:
						case RTCP::FeedbackRtp::MessageType::TLLEI:
						case RTCP::FeedbackRtp::MessageType::ECN:
						case RTCP::FeedbackRtp::MessageType::PS:
						case RTCP::FeedbackRtp::MessageType::EXT:
						default:
						{
							MS_WARN_TAG(
							    rtcp,
							    "ignoring unsupported %s Feedback packet "
							    "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
							    RTCP::FeedbackRtpPacket::MessageType2String(feedback->GetMessageType()).c_str(),
							    feedback->GetMediaSsrc(),
							    feedback->GetMediaSsrc());

							break;
						}
					}

					break;
				}

				/* RTCP coming from a remote sender which must be forwarded to the corresponding remote
				 * receivers. */

				case RTCP::Type::SR:
				{
					auto* sr = dynamic_cast<RTCP::SenderReportPacket*>(packet);
					auto it  = sr->Begin();

					// Even if Sender Report packet can only contain one report..
					for (; it != sr->End(); ++it)
					{
						auto& report = (*it);
						// Get the receiver associated to the SSRC indicated in the report.
						auto* producer = transport->GetProducer(report->GetSsrc());

						if (producer != nullptr)
						{
							this->listener->OnPeerRtcpSenderReport(this, producer, report);
						}
						else
						{
							MS_WARN_TAG(
							    rtcp,
							    "no Producer found for received Sender Report [ssrc:%" PRIu32 "]",
							    report->GetSsrc());
						}
					}

					break;
				}

				case RTCP::Type::SDES:
				{
					auto* sdes = dynamic_cast<RTCP::SdesPacket*>(packet);
					auto it    = sdes->Begin();

					for (; it != sdes->End(); ++it)
					{
						auto& chunk = (*it);
						// Get the receiver associated to the SSRC indicated in the chunk.
						auto* producer = transport->GetProducer(chunk->GetSsrc());

						if (producer == nullptr)
						{
							MS_WARN_TAG(
							    rtcp, "no Producer for received SDES chunk [ssrc:%" PRIu32 "]", chunk->GetSsrc());
						}
					}

					break;
				}

				case RTCP::Type::BYE:
				{
					MS_DEBUG_TAG(rtcp, "ignoring received RTCP BYE");

					break;
				}

				default:
				{
					MS_WARN_TAG(
					    rtcp,
					    "unhandled RTCP type received [type:%" PRIu8 "]",
					    static_cast<uint8_t>(packet->GetType()));
				}
			}

			packet = packet->GetNext();
		}
	}

	void Peer::OnProducerClosed(const RTC::Producer* producer)
	{
		MS_TRACE();

		// We must remove the closed Producer from the Transports holding it.
		for (auto& kv : this->transports)
		{
			auto transport = kv.second;

			transport->RemoveProducer(producer);
		}

		// Remove from the map.
		this->producers.erase(producer->producerId);

		// Notify the listener (Room) so it can remove this Producer from its map.
		this->listener->OnPeerProducerClosed(this, producer);
	}

	void Peer::OnConsumerClosed(RTC::Consumer* consumer)
	{
		MS_TRACE();

		// Remove from the map.
		this->consumers.erase(consumer->consumerId);

		// Notify the listener (Room) so it can remove this Consumer from its map.
		this->listener->OnPeerConsumerClosed(this, consumer);
	}

	void Peer::OnConsumerFullFrameRequired(RTC::Consumer* consumer)
	{
		MS_TRACE();

		this->listener->OnFullFrameRequired(this, consumer);
	}

	void Peer::OnTimer(Timer* /*timer*/)
	{
		uint64_t interval = RTC::RTCP::MaxVideoIntervalMs;
		uint32_t now      = DepLibUV::GetTime();

		this->SendRtcp(now);

		// Recalculate next RTCP interval.
		if (!this->consumers.empty())
		{
			// Transmission rate in kbps.
			uint32_t rate = 0;

			// Get the RTP sending rate.
			for (auto& kv : this->consumers)
			{
				auto* consumer = kv.second;

				rate += consumer->GetTransmissionRate(now) / 1000;
			}

			// Calculate bandwidth: 360 / transmission bandwidth in kbit/s
			if (rate != 0u)
				interval = 360000 / rate;

			if (interval > RTC::RTCP::MaxVideoIntervalMs)
				interval = RTC::RTCP::MaxVideoIntervalMs;
		}

		/*
		 * The interval between RTCP packets is varied randomly over the range
		 * [0.5,1.5] times the calculated interval to avoid unintended synchronization
		 * of all participants.
		 */
		interval *= static_cast<float>(Utils::Crypto::GetRandomUInt(5, 15)) / 10;
		this->timer->Start(interval);
	}
} // namespace RTC
